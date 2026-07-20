#include "../../../include/compiler/midend/pass/LoopScalarPromotionPass.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> successors;
  std::vector<std::vector<std::string>> predecessors;
};

struct Definition {
  const Instruction *instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct InductionInfo {
  std::string value;
  std::string initial;
  std::string bound;
  std::string preheader;
  std::vector<std::string> backedges;
};

struct AccessPath {
  std::string root;
  std::vector<std::string> indices;
};

struct BaseOffset {
  std::string base;
  long long offset = 0;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string exit;
  std::string preheader;
  std::unordered_set<std::string> region;
  InductionInfo induction;
  std::string type;
  std::string address;
  std::string loadResult;
  std::string loadBlock;
  std::string storeBlock;
  std::string mergeBlock;
  std::string storedValue;
};

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
  if (terminator->operands().size() == 1) {
    return {terminator->operands()[0]};
  }
  if (terminator->operands().size() == 3) {
    if (terminator->operands()[1] == terminator->operands()[2]) {
      return {terminator->operands()[1]};
    }
    return {terminator->operands()[1], terminator->operands()[2]};
  }
  return {};
}

CFG buildCFG(const Function &function) {
  CFG cfg;
  cfg.successors.resize(function.blocks().size());
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    for (const auto &target : branchTargets(function.blocks()[index])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end())
        continue;
      cfg.successors[index].push_back(target);
      cfg.predecessors[found->second].push_back(
          function.blocks()[index].name());
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function &function) {
  DefinitionMap definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
      }
    }
  }
  return definitions;
}

const BasicBlock *findBlock(const Function &function, const CFG &cfg,
                            const std::string &name) {
  const auto found = cfg.index.find(name);
  return found == cfg.index.end() ? nullptr : &function.blocks()[found->second];
}

BasicBlock *findBlock(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name)
      return &block;
  }
  return nullptr;
}

bool isUnitIncrement(const Instruction &instruction,
                     const std::string &induction) {
  if (instruction.opcode() != "add" || !instruction.hasResult() ||
      instruction.operands().size() != 2) {
    return false;
  }
  long long literal = 0;
  return (instruction.operands()[0] == induction &&
          detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
          literal == 1) ||
         (instruction.operands()[1] == induction &&
          detail::parseIntegerLiteral(instruction.operands()[0], literal) &&
          literal == 1);
}

std::optional<std::unordered_set<std::string>>
collectRegion(const CFG &cfg, const std::string &body,
              const std::string &header, const std::string &exit) {
  std::unordered_set<std::string> region;
  std::vector<std::string> work{body};
  while (!work.empty()) {
    const auto current = std::move(work.back());
    work.pop_back();
    if (current == header || !region.insert(current).second)
      continue;
    if (current == exit)
      return std::nullopt;
    const auto found = cfg.index.find(current);
    if (found == cfg.index.end())
      return std::nullopt;
    for (const auto &successor : cfg.successors[found->second]) {
      if (successor == exit)
        return std::nullopt;
      if (successor != header)
        work.push_back(successor);
    }
  }
  return region;
}

std::optional<InductionInfo>
findInduction(const BasicBlock &header, const CFG &cfg,
              const DefinitionMap &definitions,
              const std::unordered_set<std::string> &region,
              const std::string &body, const std::string &exit) {
  const auto *terminator = header.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3 || terminator->operands()[1] != body ||
      terminator->operands()[2] != exit) {
    return std::nullopt;
  }
  const auto condition = definitions.find(terminator->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.block != header.name() ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto &induction = condition->second.instruction->operands()[0];
  const auto &bound = condition->second.instruction->operands()[1];
  const Instruction *phi = nullptr;
  for (const auto &instruction : header.instructions()) {
    if (instruction.opcode() == "phi" && instruction.result() == induction) {
      phi = &instruction;
      break;
    }
  }
  if (phi == nullptr || phi->operands().size() < 5 ||
      phi->operands().size() % 2 == 0 || phi->operands()[0] != "int") {
    return std::nullopt;
  }

  InductionInfo info;
  info.value = induction;
  info.bound = bound;
  for (std::size_t index = 1; index + 1 < phi->operands().size(); index += 2) {
    const auto &value = phi->operands()[index];
    const auto &predecessor = phi->operands()[index + 1];
    if (region.find(predecessor) == region.end()) {
      if (!info.preheader.empty())
        return std::nullopt;
      info.preheader = predecessor;
      info.initial = value;
      continue;
    }
    const auto increment = definitions.find(value);
    if (increment == definitions.end() ||
        region.find(increment->second.block) == region.end() ||
        !isUnitIncrement(*increment->second.instruction, induction)) {
      return std::nullopt;
    }
    info.backedges.push_back(predecessor);
  }
  if (info.preheader.empty() || info.backedges.empty())
    return std::nullopt;

  const auto headerFound = cfg.index.find(header.name());
  if (headerFound == cfg.index.end())
    return std::nullopt;
  std::unordered_set<std::string> expected(info.backedges.begin(),
                                           info.backedges.end());
  expected.insert(info.preheader);
  const std::unordered_set<std::string> actual(
      cfg.predecessors[headerFound->second].begin(),
      cfg.predecessors[headerFound->second].end());
  if (actual != expected)
    return std::nullopt;
  return info;
}

std::optional<AccessPath> accessPath(const std::string &address,
                                     const DefinitionMap &definitions) {
  AccessPath path;
  path.root = address;
  std::unordered_set<std::string> visited;
  while (!path.root.empty() && path.root.front() == '%' &&
         visited.insert(path.root).second) {
    const auto found = definitions.find(path.root);
    if (found == definitions.end())
      break;
    const auto &instruction = *found->second.instruction;
    const auto &opcode = instruction.opcode();
    if (opcode != "getelementptr" && opcode != "getelementptr.linear" &&
        opcode != "gep" && opcode != "gep.linear") {
      break;
    }
    if (instruction.operands().size() != 2)
      return std::nullopt;
    path.indices.insert(path.indices.begin(), instruction.operands()[1]);
    path.root = instruction.operands()[0];
  }
  return path.indices.empty() ? std::nullopt : std::optional{path};
}

BaseOffset baseOffset(const std::string &value,
                      const DefinitionMap &definitions, unsigned depth = 0) {
  if (depth >= 8)
    return BaseOffset{value, 0};
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return BaseOffset{value, 0};
  const auto &instruction = *found->second.instruction;
  if (instruction.opcode() != "add" || instruction.operands().size() != 2) {
    return BaseOffset{value, 0};
  }
  long long literal = 0;
  if (detail::parseIntegerLiteral(instruction.operands()[1], literal)) {
    auto result = baseOffset(instruction.operands()[0], definitions, depth + 1);
    result.offset += literal;
    return result;
  }
  if (detail::parseIntegerLiteral(instruction.operands()[0], literal)) {
    auto result = baseOffset(instruction.operands()[1], definitions, depth + 1);
    result.offset += literal;
    return result;
  }
  return BaseOffset{value, 0};
}

bool proveUnequal(const std::string &lhs, const std::string &rhs,
                  const InductionInfo &induction,
                  const DefinitionMap &definitions) {
  if (lhs == rhs)
    return false;
  long long lhsLiteral = 0;
  long long rhsLiteral = 0;
  if (detail::parseIntegerLiteral(lhs, lhsLiteral) &&
      detail::parseIntegerLiteral(rhs, rhsLiteral)) {
    return lhsLiteral != rhsLiteral;
  }
  if ((lhs == induction.value && rhs == induction.bound) ||
      (rhs == induction.value && lhs == induction.bound)) {
    return true;
  }

  const auto lhsOffset = baseOffset(lhs, definitions);
  const auto rhsOffset = baseOffset(rhs, definitions);
  if (lhsOffset.base == rhsOffset.base) {
    return lhsOffset.offset != rhsOffset.offset;
  }

  const auto initial = baseOffset(induction.initial, definitions);
  const auto provesPastInitialBase = [&](const BaseOffset &varying,
                                         const BaseOffset &fixed) {
    if (varying.base != induction.value || fixed.base != initial.base) {
      return false;
    }
    return initial.offset > 0 && varying.offset >= 0 &&
           fixed.offset < initial.offset + varying.offset;
  };
  return provesPastInitialBase(lhsOffset, rhsOffset) ||
         provesPastInitialBase(rhsOffset, lhsOffset);
}

bool distinctRoots(const std::string &lhs, const std::string &rhs,
                   const DefinitionMap &definitions) {
  if (lhs == rhs)
    return false;
  if (!lhs.empty() && !rhs.empty() && lhs.front() == '@' &&
      rhs.front() == '@') {
    return true;
  }
  const auto lhsDef = definitions.find(lhs);
  const auto rhsDef = definitions.find(rhs);
  return lhsDef != definitions.end() && rhsDef != definitions.end() &&
         lhsDef->second.instruction->opcode() == "alloca" &&
         rhsDef->second.instruction->opcode() == "alloca";
}

bool proveNoAlias(const std::string &lhs, const std::string &rhs,
                  const InductionInfo &induction,
                  const DefinitionMap &definitions) {
  const auto lhsPath = accessPath(lhs, definitions);
  const auto rhsPath = accessPath(rhs, definitions);
  if (!lhsPath.has_value() || !rhsPath.has_value())
    return false;
  if (distinctRoots(lhsPath->root, rhsPath->root, definitions))
    return true;
  if (lhsPath->root != rhsPath->root ||
      lhsPath->indices.size() != rhsPath->indices.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhsPath->indices.size(); ++index) {
    if (proveUnequal(lhsPath->indices[index], rhsPath->indices[index],
                     induction, definitions)) {
      return true;
    }
  }
  return false;
}

bool valueUsedOutsideRegion(const Function &function,
                            const Candidate &candidate) {
  for (const auto &block : function.blocks()) {
    const bool inside =
        candidate.region.find(block.name()) != candidate.region.end();
    for (const auto &instruction : block.instructions()) {
      if (inside)
        continue;
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    candidate.loadResult) != instruction.operands().end()) {
        return true;
      }
    }
  }
  return false;
}

bool isSpeculatableUpdate(const Instruction &instruction) {
  static const std::unordered_set<std::string> opcodes{
      "add", "sub",    "mul",  "and",  "or",   "xor",
      "neg", "select", "cast", "zext", "sext", "trunc"};
  return instruction.hasResult() &&
         opcodes.find(instruction.opcode()) != opcodes.end();
}

std::optional<Candidate> findCandidate(const Function &function) {
  const bool trace =
      std::getenv("COMPILER_LOOP_SCALAR_PROMOTION_STATS") != nullptr;
  const auto report = [&](const BasicBlock &header, const char *reason) {
    if (trace) {
      std::cerr << "loop-scalar-promotion: " << function.name() << ':'
                << header.name() << ": " << reason << '\n';
    }
  };
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  for (const auto &header : function.blocks()) {
    const auto *terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto body = terminator->operands()[1];
    const auto exit = terminator->operands()[2];
    const auto region = collectRegion(cfg, body, header.name(), exit);
    if (!region.has_value() || region->size() < 2)
      continue;
    const auto induction =
        findInduction(header, cfg, definitions, *region, body, exit);
    if (!induction.has_value()) {
      report(header, "induction");
      continue;
    }
    const auto *preheader = findBlock(function, cfg, induction->preheader);
    const auto *exitBlock = findBlock(function, cfg, exit);
    if (preheader == nullptr || preheader->terminator() == nullptr ||
        preheader->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        exitBlock == nullptr ||
        (!exitBlock->instructions().empty() &&
         exitBlock->instructions().front().opcode() == "phi")) {
      report(header, "preheader-or-exit");
      continue;
    }

    const Instruction *targetLoad = nullptr;
    const Instruction *targetStore = nullptr;
    std::string storeBlock;
    bool rejected = false;
    for (const auto &blockName : *region) {
      const auto *block = findBlock(function, cfg, blockName);
      if (block == nullptr || block->terminator() == nullptr) {
        rejected = true;
        break;
      }
      for (const auto &instruction : block->instructions()) {
        if (instruction.opcode() == "call") {
          rejected = true;
          break;
        }
        if (instruction.opcode() == "store") {
          if (instruction.operands().size() != 3 || targetStore != nullptr) {
            rejected = true;
            break;
          }
          targetStore = &instruction;
          storeBlock = blockName;
        }
      }
      if (rejected)
        break;
    }
    if (rejected || targetStore == nullptr) {
      report(header, "store-effects");
      continue;
    }
    const auto &storeOperands = targetStore->operands();

    for (const auto &blockName : *region) {
      const auto *block = findBlock(function, cfg, blockName);
      for (const auto &instruction : block->instructions()) {
        if (instruction.opcode() != "load" ||
            instruction.operands().size() != 2) {
          continue;
        }
        if (instruction.operands()[1] == storeOperands[2]) {
          if (targetLoad != nullptr ||
              instruction.operands()[0] != storeOperands[0]) {
            rejected = true;
            break;
          }
          targetLoad = &instruction;
        } else if (!proveNoAlias(storeOperands[2], instruction.operands()[1],
                                 *induction, definitions)) {
          rejected = true;
          break;
        }
      }
      if (rejected)
        break;
    }
    if (rejected || targetLoad == nullptr) {
      report(header, "load-alias");
      continue;
    }

    const auto *loadBlock =
        findBlock(function, cfg, definitions.at(targetLoad->result()).block);
    const auto *updateBlock = findBlock(function, cfg, storeBlock);
    if (loadBlock == nullptr || updateBlock == nullptr ||
        loadBlock->terminator() == nullptr ||
        loadBlock->terminator()->opcode() != "br" ||
        loadBlock->terminator()->operands().size() != 3 ||
        updateBlock->terminator() == nullptr) {
      if (trace) {
        std::cerr << "  load="
                  << (loadBlock == nullptr ? "<null>" : loadBlock->name())
                  << " store=" << storeBlock << " header=" << header.name();
        if (loadBlock != nullptr && loadBlock->terminator() != nullptr) {
          std::cerr << " load-br=";
          for (const auto &operand : loadBlock->terminator()->operands()) {
            std::cerr << operand << ',';
          }
        }
        if (updateBlock != nullptr && updateBlock->terminator() != nullptr) {
          std::cerr << " store-br=";
          for (const auto &operand : updateBlock->terminator()->operands()) {
            std::cerr << operand << ',';
          }
        }
        std::cerr << '\n';
      }
      report(header, "update-cfg");
      continue;
    }
    const auto &loadTargets = loadBlock->terminator()->operands();
    std::string mergeBlock;
    if (loadTargets[1] == storeBlock) {
      mergeBlock = loadTargets[2];
    } else if (loadTargets[2] == storeBlock) {
      mergeBlock = loadTargets[1];
    } else {
      report(header, "update-edge");
      continue;
    }
    if (mergeBlock == header.name()) {
      if (updateBlock->terminator()->operands() !=
          std::vector<std::string>{header.name()}) {
        report(header, "direct-update-edge");
        continue;
      }
    } else {
      const auto *merge = findBlock(function, cfg, mergeBlock);
      const auto mergeFound = cfg.index.find(mergeBlock);
      const std::unordered_set<std::string> expectedMergePredecessors{
          loadBlock->name(), storeBlock};
      const std::unordered_set<std::string> actualMergePredecessors =
          mergeFound == cfg.index.end()
              ? std::unordered_set<std::string>{}
              : std::unordered_set<std::string>(
                    cfg.predecessors[mergeFound->second].begin(),
                    cfg.predecessors[mergeFound->second].end());
      if (updateBlock->terminator()->operands() !=
              std::vector<std::string>{mergeBlock} ||
          merge == nullptr || merge->terminator() == nullptr ||
          merge->terminator()->operands() !=
              std::vector<std::string>{header.name()} ||
          actualMergePredecessors != expectedMergePredecessors ||
          std::any_of(merge->instructions().begin(),
                      merge->instructions().end() - 1,
                      [](const Instruction &instruction) {
                        return instruction.opcode() == "load" ||
                               instruction.opcode() == "store" ||
                               instruction.opcode() == "call";
                      })) {
        report(header, "merge-update-edge");
        continue;
      }
    }
    const auto backedgeBlock =
        mergeBlock == header.name() ? loadBlock->name() : mergeBlock;
    if (std::find(induction->backedges.begin(), induction->backedges.end(),
                  backedgeBlock) == induction->backedges.end()) {
      report(header, "backedges");
      continue;
    }

    const auto addressDefinition = definitions.find(storeOperands[2]);
    if (addressDefinition != definitions.end() &&
        (addressDefinition->second.block == header.name() ||
         region->find(addressDefinition->second.block) != region->end())) {
      continue;
    }

    Candidate candidate{header.name(),
                        body,
                        exit,
                        induction->preheader,
                        *region,
                        *induction,
                        storeOperands[0],
                        storeOperands[2],
                        targetLoad->result(),
                        loadBlock->name(),
                        storeBlock,
                        mergeBlock,
                        storeOperands[1]};
    if (!valueUsedOutsideRegion(function, candidate))
      return candidate;
    report(header, "load-escapes-loop");
  }
  return std::nullopt;
}

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem, bool value) {
  std::string candidate = value ? "%" + stem : stem;
  std::size_t suffix = 0;
  while (!names.insert(candidate).second) {
    candidate = (value ? "%" : "") + stem + "." + std::to_string(++suffix);
  }
  return candidate;
}

bool transform(Function &function, const Candidate &candidate) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        names.insert(instruction.result());
    }
  }
  const auto guardName = freshName(names, "loop.scalar.guard", false);
  const auto initName = freshName(names, "loop.scalar.init", false);
  const auto commitName = freshName(names, "loop.scalar.commit", false);
  const auto enter = freshName(names, "loop.scalar.enter", true);
  const auto initial = freshName(names, "loop.scalar.initial", true);
  const auto current = freshName(names, "loop.scalar.current", true);
  const auto next = freshName(names, "loop.scalar.next", true);
  const auto selected = freshName(names, "loop.scalar.selected", true);

  auto *preheader = findBlock(function, candidate.preheader);
  auto *header = findBlock(function, candidate.header);
  if (preheader == nullptr || header == nullptr ||
      header->terminator() == nullptr) {
    return false;
  }
  preheader->instructions().back() = Instruction{"br", {guardName}};

  for (auto &instruction : header->instructions()) {
    if (instruction.opcode() != "phi")
      break;
    auto operands = instruction.operands();
    for (std::size_t index = 2; index < operands.size(); index += 2) {
      if (operands[index] == candidate.preheader)
        operands[index] = initName;
    }
    instruction = Instruction{"phi", std::move(operands), instruction.result()};
  }

  std::unordered_map<std::string, std::string> aliases{
      {candidate.loadResult, current}};
  std::unordered_map<std::string, std::string> availableLoads;
  if (const auto *loadBlock = findBlock(function, candidate.loadBlock)) {
    for (const auto &instruction : loadBlock->instructions()) {
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          instruction.operands().size() == 2 &&
          instruction.result() != candidate.loadResult) {
        availableLoads.emplace(instruction.operands()[0] + "\n" +
                                   instruction.operands()[1],
                               instruction.result());
      }
    }
  }
  for (const auto &blockName : candidate.region) {
    auto *block = findBlock(function, blockName);
    if (block == nullptr)
      return false;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block->instructions().size());
    for (const auto &instruction : block->instructions()) {
      if (instruction.result() == candidate.loadResult)
        continue;
      if (blockName == candidate.storeBlock && instruction.opcode() == "load" &&
          instruction.hasResult() && instruction.operands().size() == 2) {
        const auto available = availableLoads.find(
            instruction.operands()[0] + "\n" + instruction.operands()[1]);
        if (available != availableLoads.end()) {
          aliases[instruction.result()] = available->second;
          continue;
        }
      }
      if (instruction.opcode() == "store" &&
          instruction.operands().size() == 3 &&
          instruction.operands()[2] == candidate.address) {
        continue;
      }
      rewritten.emplace_back(
          instruction.opcode(),
          detail::rewriteOperands(instruction.operands(), aliases),
          instruction.result());
    }
    block->instructions() = std::move(rewritten);
  }
  const auto storedValue = detail::resolveAlias(candidate.storedValue, aliases);

  std::string backedgeValue = current;
  bool ifConverted = false;
  if (candidate.mergeBlock != candidate.header) {
    auto *loadBlock = findBlock(function, candidate.loadBlock);
    auto *updateBlock = findBlock(function, candidate.storeBlock);
    if (loadBlock != nullptr && updateBlock != nullptr &&
        loadBlock->terminator() != nullptr &&
        loadBlock->terminator()->operands().size() == 3 &&
        updateBlock->terminator() != nullptr &&
        std::all_of(updateBlock->instructions().begin(),
                    updateBlock->instructions().end() - 1,
                    isSpeculatableUpdate)) {
      const auto condition = loadBlock->terminator()->operands()[0];
      const bool updateOnTrue =
          loadBlock->terminator()->operands()[1] == candidate.storeBlock;
      std::vector<Instruction> moved(updateBlock->instructions().begin(),
                                     updateBlock->instructions().end() - 1);
      auto insertUpdate = loadBlock->instructions().end() - 1;
      loadBlock->instructions().insert(insertUpdate, moved.begin(),
                                       moved.end());
      insertUpdate = loadBlock->instructions().end() - 1;
      loadBlock->instructions().insert(
          insertUpdate,
          Instruction{"select",
                      {condition, updateOnTrue ? storedValue : current,
                       updateOnTrue ? current : storedValue},
                      selected});
      loadBlock->instructions().back() =
          Instruction{"br", {candidate.mergeBlock}};
      for (auto block = function.blocks().begin();
           block != function.blocks().end(); ++block) {
        if (block->name() == candidate.storeBlock) {
          function.blocks().erase(block);
          break;
        }
      }
      backedgeValue = selected;
      ifConverted = true;
    }
  }
  if (candidate.mergeBlock != candidate.header && !ifConverted) {
    auto *merge = findBlock(function, candidate.mergeBlock);
    if (merge == nullptr)
      return false;
    merge->instructions().insert(
        merge->instructions().begin(),
        Instruction{"phi",
                    {candidate.type, current, candidate.loadBlock, storedValue,
                     candidate.storeBlock},
                    next});
    backedgeValue = next;
  }

  header = findBlock(function, candidate.header);
  if (header == nullptr || header->terminator() == nullptr)
    return false;
  std::vector<std::string> phiOperands{candidate.type, initial, initName};
  for (const auto &backedge : candidate.induction.backedges) {
    if (candidate.mergeBlock == candidate.header) {
      phiOperands.push_back(backedge == candidate.storeBlock ? storedValue
                                                             : current);
    } else {
      phiOperands.push_back(backedge == candidate.mergeBlock ? backedgeValue
                                                             : current);
    }
    phiOperands.push_back(backedge);
  }
  auto insert = header->instructions().begin();
  while (insert != header->instructions().end() && insert->opcode() == "phi") {
    ++insert;
  }
  header->instructions().insert(
      insert, Instruction{"phi", std::move(phiOperands), current});

  auto headerBranch = header->instructions().back().operands();
  headerBranch[2] = commitName;
  header->instructions().back() = Instruction{"br", std::move(headerBranch)};

  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"icmp.slt",
                  {candidate.induction.initial, candidate.induction.bound},
                  enter});
  guard.addInstruction(Instruction{"br", {enter, initName, candidate.exit}});

  BasicBlock init{initName};
  init.addInstruction(
      Instruction{"load", {candidate.type, candidate.address}, initial});
  init.addInstruction(Instruction{"br", {candidate.header}});

  BasicBlock commit{commitName};
  commit.addInstruction(
      Instruction{"store", {candidate.type, current, candidate.address}});
  commit.addInstruction(Instruction{"br", {candidate.exit}});

  function.addBlock(std::move(guard));
  function.addBlock(std::move(init));
  function.addBlock(std::move(commit));
  return true;
}

} // namespace

void LoopScalarPromotionPass::run(compiler::ir::core::Module &module) {
  for (auto &function : module.functions()) {
    while (const auto candidate = findCandidate(function)) {
      if (!transform(function, *candidate))
        break;
    }
  }
}

} // namespace compiler::midend::pass
