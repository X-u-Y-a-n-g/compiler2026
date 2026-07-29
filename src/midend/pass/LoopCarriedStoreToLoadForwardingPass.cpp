#include "../../../include/compiler/midend/pass/LoopCarriedStoreToLoadForwardingPass.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

struct AddressPath {
  std::string root;
  std::vector<std::string> indices;
  std::vector<Instruction> chain;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string preheader;
  std::string exit;
  std::string induction;
  std::string initial;
  std::string bound;
  std::string type;
  std::string loadResult;
  std::string storedValue;
  long long storeOffset = 0;
  AddressPath storePath;
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

bool isGep(const Instruction &instruction) {
  const auto &opcode = instruction.opcode();
  return opcode == "getelementptr" || opcode == "getelementptr.linear" ||
         opcode == "gep" || opcode == "gep.linear";
}

std::optional<AddressPath>
addressPathWithinBody(const std::string &address, const std::string &body,
                      const DefinitionMap &definitions) {
  AddressPath path;
  path.root = address;
  std::unordered_set<std::string> visited;
  while (!path.root.empty() && path.root.front() == '%' &&
         visited.insert(path.root).second) {
    const auto found = definitions.find(path.root);
    if (found == definitions.end() || found->second.block != body)
      break;
    const auto &instruction = *found->second.instruction;
    if (!isGep(instruction) || instruction.operands().size() != 2) {
      return std::nullopt;
    }
    path.indices.insert(path.indices.begin(), instruction.operands()[1]);
    path.chain.insert(path.chain.begin(), instruction);
    path.root = instruction.operands()[0];
  }
  if (path.chain.empty())
    return std::nullopt;
  return path;
}

std::optional<long long> affineOffsetFrom(const std::string &value,
                                          const std::string &induction,
                                          const DefinitionMap &definitions,
                                          unsigned depth = 0) {
  if (value == induction)
    return 0;
  if (depth >= 8)
    return std::nullopt;
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return std::nullopt;
  const auto &instruction = *found->second.instruction;
  if ((instruction.opcode() != "add" && instruction.opcode() != "sub") ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }

  long long literal = 0;
  std::optional<long long> base;
  __int128 adjusted = 0;
  if (detail::parseIntegerLiteral(instruction.operands()[1], literal)) {
    base = affineOffsetFrom(instruction.operands()[0], induction, definitions,
                            depth + 1);
    if (!base.has_value())
      return std::nullopt;
    adjusted = static_cast<__int128>(*base) +
               (instruction.opcode() == "add" ? literal : -literal);
  } else if (instruction.opcode() == "add" &&
             detail::parseIntegerLiteral(instruction.operands()[0], literal)) {
    base = affineOffsetFrom(instruction.operands()[1], induction, definitions,
                            depth + 1);
    if (!base.has_value())
      return std::nullopt;
    adjusted = static_cast<__int128>(*base) + literal;
  } else {
    return std::nullopt;
  }
  if (adjusted < std::numeric_limits<long long>::min() ||
      adjusted > std::numeric_limits<long long>::max() ||
      adjusted < std::numeric_limits<std::int32_t>::min() ||
      adjusted > std::numeric_limits<std::int32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<long long>(adjusted);
}

bool dependsOn(const std::string &value, const std::string &induction,
               const DefinitionMap &definitions,
               std::unordered_set<std::string> &visiting, unsigned depth = 0) {
  if (value == induction)
    return true;
  if (depth >= 16 || value.empty() || value.front() != '%' ||
      !visiting.insert(value).second) {
    return false;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return false;
  for (const auto &operand : found->second.instruction->operands()) {
    if (dependsOn(operand, induction, definitions, visiting, depth + 1)) {
      return true;
    }
  }
  return false;
}

bool dependsOn(const std::string &value, const std::string &induction,
               const DefinitionMap &definitions) {
  std::unordered_set<std::string> visiting;
  return dependsOn(value, induction, definitions, visiting);
}

bool isAvailableBeforeLoop(const std::string &value, const std::string &header,
                           const std::string &body,
                           const DefinitionMap &definitions) {
  if (value.empty() || value.front() != '%')
    return true;
  const auto found = definitions.find(value);
  return found == definitions.end() ||
         (found->second.block != header && found->second.block != body);
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

std::optional<Candidate> findCandidate(const Function &function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  for (const auto &header : function.blocks()) {
    const auto *terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto &conditionName = terminator->operands()[0];
    const auto &bodyName = terminator->operands()[1];
    const auto &exitName = terminator->operands()[2];
    const auto condition = definitions.find(conditionName);
    const auto *body = findBlock(function, cfg, bodyName);
    if (condition == definitions.end() ||
        condition->second.block != header.name() ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2 ||
        body == nullptr || body->terminator() == nullptr ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const auto &induction = condition->second.instruction->operands()[0];
    const auto &bound = condition->second.instruction->operands()[1];
    const Instruction *inductionPhi = nullptr;
    for (const auto &instruction : header.instructions()) {
      if (instruction.opcode() == "phi" && instruction.result() == induction) {
        inductionPhi = &instruction;
        break;
      }
    }
    if (inductionPhi == nullptr || inductionPhi->operands().size() != 5 ||
        inductionPhi->operands()[0] != "int") {
      continue;
    }

    std::string preheader;
    std::string initial;
    std::string backedgeValue;
    for (std::size_t index = 1; index < 5; index += 2) {
      if (inductionPhi->operands()[index + 1] == bodyName) {
        backedgeValue = inductionPhi->operands()[index];
      } else {
        preheader = inductionPhi->operands()[index + 1];
        initial = inductionPhi->operands()[index];
      }
    }
    const auto increment = definitions.find(backedgeValue);
    const auto *preheaderBlock = findBlock(function, cfg, preheader);
    const auto headerIndex = cfg.index.find(header.name());
    if (preheader.empty() || initial.empty() ||
        increment == definitions.end() || increment->second.block != bodyName ||
        !isUnitIncrement(*increment->second.instruction, induction) ||
        preheaderBlock == nullptr || preheaderBlock->terminator() == nullptr ||
        preheaderBlock->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        headerIndex == cfg.index.end()) {
      continue;
    }
    const std::unordered_set<std::string> expectedPredecessors{preheader,
                                                               bodyName};
    const std::unordered_set<std::string> actualPredecessors(
        cfg.predecessors[headerIndex->second].begin(),
        cfg.predecessors[headerIndex->second].end());
    if (actualPredecessors != expectedPredecessors ||
        dependsOn(bound, induction, definitions) ||
        !isAvailableBeforeLoop(initial, header.name(), bodyName, definitions) ||
        !isAvailableBeforeLoop(bound, header.name(), bodyName, definitions)) {
      continue;
    }

    const Instruction *store = nullptr;
    std::size_t storeIndex = 0;
    bool rejected = false;
    for (std::size_t index = 0; index < body->instructions().size(); ++index) {
      const auto &instruction = body->instructions()[index];
      if (instruction.opcode() == "call") {
        rejected = true;
        break;
      }
      if (instruction.opcode() == "store") {
        if (store != nullptr || instruction.operands().size() != 3 ||
            instruction.operands()[0] != "int") {
          rejected = true;
          break;
        }
        store = &instruction;
        storeIndex = index;
      }
    }
    if (rejected || store == nullptr)
      continue;

    const auto storePath =
        addressPathWithinBody(store->operands()[2], bodyName, definitions);
    if (!storePath.has_value() ||
        !isAvailableBeforeLoop(storePath->root, header.name(), bodyName,
                               definitions)) {
      continue;
    }
    for (std::size_t index = 0; index + 1 < storePath->indices.size();
         ++index) {
      if (!isAvailableBeforeLoop(storePath->indices[index], header.name(),
                                 bodyName, definitions)) {
        rejected = true;
        break;
      }
    }
    if (rejected)
      continue;
    const auto storeOffset =
        affineOffsetFrom(storePath->indices.back(), induction, definitions);
    if (!storeOffset.has_value())
      continue;

    const Instruction *load = nullptr;
    for (std::size_t index = 0; index < storeIndex; ++index) {
      const auto &instruction = body->instructions()[index];
      if (instruction.opcode() != "load" || !instruction.hasResult() ||
          instruction.operands().size() != 2 ||
          instruction.operands()[0] != store->operands()[0]) {
        continue;
      }
      const auto loadPath = addressPathWithinBody(instruction.operands()[1],
                                                  bodyName, definitions);
      if (!loadPath.has_value() || loadPath->root != storePath->root ||
          loadPath->indices.size() != storePath->indices.size() ||
          !std::equal(loadPath->indices.begin(), loadPath->indices.end() - 1,
                      storePath->indices.begin())) {
        continue;
      }
      const auto loadOffset =
          affineOffsetFrom(loadPath->indices.back(), induction, definitions);
      const __int128 expectedLoadOffset =
          static_cast<__int128>(*storeOffset) - 1;
      if (!loadOffset.has_value() ||
          expectedLoadOffset < std::numeric_limits<std::int32_t>::min() ||
          expectedLoadOffset > std::numeric_limits<std::int32_t>::max() ||
          *loadOffset != static_cast<long long>(expectedLoadOffset)) {
        continue;
      }
      if (load != nullptr) {
        rejected = true;
        break;
      }
      load = &instruction;
    }
    if (rejected || load == nullptr)
      continue;

    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (block.name() != bodyName &&
            std::find(instruction.operands().begin(),
                      instruction.operands().end(),
                      load->result()) != instruction.operands().end()) {
          rejected = true;
          break;
        }
      }
      if (rejected)
        break;
    }
    if (rejected)
      continue;

    return Candidate{header.name(),  bodyName,
                     preheader,      exitName,
                     induction,      initial,
                     bound,          store->operands()[0],
                     load->result(), store->operands()[1],
                     *storeOffset,   *storePath};
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
  const auto guardName = freshName(names, "store.load.guard", false);
  const auto initName = freshName(names, "store.load.init", false);
  const auto canEnter = freshName(names, "store.load.can.enter", true);
  const auto initialIndex = freshName(names, "store.load.initial.index", true);
  const auto initialValue = freshName(names, "store.load.initial", true);
  const auto currentValue = freshName(names, "store.load.current", true);

  auto *preheader = findBlock(function, candidate.preheader);
  auto *header = findBlock(function, candidate.header);
  auto *body = findBlock(function, candidate.body);
  if (preheader == nullptr || header == nullptr || body == nullptr ||
      preheader->terminator() == nullptr) {
    return false;
  }
  preheader->instructions().back() = Instruction{"br", {guardName}};

  for (auto &instruction : header->instructions()) {
    if (instruction.opcode() != "phi")
      break;
    std::vector<std::string> operands;
    operands.push_back(instruction.operands()[0]);
    bool replaced = false;
    for (std::size_t index = 1; index + 1 < instruction.operands().size();
         index += 2) {
      const auto &value = instruction.operands()[index];
      const auto &predecessor = instruction.operands()[index + 1];
      if (predecessor == candidate.preheader) {
        operands.push_back(value);
        operands.push_back(guardName);
        operands.push_back(value);
        operands.push_back(initName);
        replaced = true;
      } else {
        operands.push_back(value);
        operands.push_back(predecessor);
      }
    }
    if (!replaced)
      return false;
    instruction = Instruction{"phi", std::move(operands), instruction.result()};
  }

  std::unordered_map<std::string, std::string> aliases{
      {candidate.loadResult, currentValue}};
  std::vector<Instruction> rewritten;
  rewritten.reserve(body->instructions().size());
  for (const auto &instruction : body->instructions()) {
    if (instruction.result() == candidate.loadResult)
      continue;
    rewritten.emplace_back(
        instruction.opcode(),
        detail::rewriteOperands(instruction.operands(), aliases),
        instruction.result());
  }
  body->instructions() = std::move(rewritten);
  const auto storedValue = detail::resolveAlias(candidate.storedValue, aliases);

  header = findBlock(function, candidate.header);
  if (header == nullptr)
    return false;
  auto insert = header->instructions().begin();
  while (insert != header->instructions().end() && insert->opcode() == "phi") {
    ++insert;
  }
  header->instructions().insert(
      insert, Instruction{"phi",
                          {candidate.type, "0", guardName, initialValue,
                           initName, storedValue, candidate.body},
                          currentValue});

  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"icmp.slt", {candidate.initial, candidate.bound}, canEnter});
  guard.addInstruction(
      Instruction{"br", {canEnter, initName, candidate.header}});

  BasicBlock init{initName};
  std::string initIndexValue = candidate.initial;
  const __int128 initialOffset =
      static_cast<__int128>(candidate.storeOffset) - 1;
  if (initialOffset != 0) {
    long long initialLiteral = 0;
    if (detail::parseIntegerLiteral(candidate.initial, initialLiteral) &&
        initialLiteral >= std::numeric_limits<std::int32_t>::min() &&
        initialLiteral <= std::numeric_limits<std::int32_t>::max()) {
      const std::uint64_t bits =
          (static_cast<std::uint64_t>(
               static_cast<std::uint32_t>(initialLiteral)) +
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(
               static_cast<long long>(initialOffset)))) &
          0xffffffffULL;
      const long long folded =
          bits <= static_cast<std::uint64_t>(
                      std::numeric_limits<std::int32_t>::max())
              ? static_cast<long long>(bits)
              : static_cast<long long>(bits) - (1LL << 32);
      initIndexValue = std::to_string(folded);
    } else if (initialOffset > 0) {
      init.addInstruction(
          Instruction{"add",
                      {candidate.initial,
                       std::to_string(static_cast<long long>(initialOffset))},
                      initialIndex});
      initIndexValue = initialIndex;
    } else {
      init.addInstruction(
          Instruction{"sub",
                      {candidate.initial,
                       std::to_string(static_cast<long long>(-initialOffset))},
                      initialIndex});
      initIndexValue = initialIndex;
    }
  }

  std::string address = candidate.storePath.root;
  for (std::size_t index = 0; index < candidate.storePath.chain.size();
       ++index) {
    const auto result = freshName(names, "store.load.initial.address", true);
    const auto &original = candidate.storePath.chain[index];
    const auto &gepIndex = index + 1 == candidate.storePath.chain.size()
                               ? initIndexValue
                               : original.operands()[1];
    init.addInstruction(
        Instruction{original.opcode(), {address, gepIndex}, result});
    address = result;
  }
  init.addInstruction(
      Instruction{"load", {candidate.type, address}, initialValue});
  init.addInstruction(Instruction{"br", {candidate.header}});

  function.addBlock(std::move(guard));
  function.addBlock(std::move(init));
  return true;
}

} // namespace

void LoopCarriedStoreToLoadForwardingPass::run(
    compiler::ir::core::Module &module) {
  std::size_t transformed = 0;
  for (auto &function : module.functions()) {
    while (const auto candidate = findCandidate(function)) {
      if (!transform(function, *candidate))
        break;
      ++transformed;
    }
  }
  if (std::getenv("COMPILER_LOOP_CARRIED_STORE_LOAD_FORWARDING_STATS") !=
      nullptr) {
    std::cerr << "loop-carried-store-load-forwarding: transformed="
              << transformed << '\n';
  }
}

} // namespace compiler::midend::pass
