#include "../../../include/compiler/midend/pass/NestedInvariantReductionPass.h"

#include <algorithm>
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
  const Instruction* instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct Candidate {
  std::string header;
  std::string bodyEntry;
  std::string exit;
  std::string latch;
  std::string preheader;
  std::string counterSlot;
  std::string reductionSlot;
  std::string counterValue;
  std::string originalCondition;
  std::string count;
};

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") return {};
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

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.successors.resize(function.blocks().size());
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    for (const auto& target : branchTargets(function.blocks()[index])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end()) continue;
      cfg.successors[index].push_back(target);
      cfg.predecessors[found->second].push_back(function.blocks()[index].name());
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
      }
    }
  }
  return definitions;
}

const BasicBlock* findBlock(const Function& function, const CFG& cfg,
                            const std::string& name) {
  const auto found = cfg.index.find(name);
  return found == cfg.index.end() ? nullptr : &function.blocks()[found->second];
}

BasicBlock* findBlock(Function& function, const std::string& name) {
  for (auto& block : function.blocks()) {
    if (block.name() == name) return &block;
  }
  return nullptr;
}

bool isLoadFrom(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "load" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         instruction.operands()[0] == "int" &&
         instruction.operands()[1] == slot;
}

bool isStoreTo(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "store" &&
         instruction.operands().size() == 3 &&
         instruction.operands()[0] == "int" &&
         instruction.operands()[2] == slot;
}

bool blockStoresZeroTo(const BasicBlock& block, const std::string& slot) {
  return std::any_of(block.instructions().begin(), block.instructions().end(),
                     [&](const Instruction& instruction) {
                       return isStoreTo(instruction, slot) &&
                              detail::isZero(instruction.operands()[1]);
                     });
}

std::optional<std::pair<std::string, std::string>> unitIncrementSlot(
    const BasicBlock& block, const DefinitionMap& definitions) {
  for (const auto& store : block.instructions()) {
    if (store.opcode() != "store" || store.operands().size() != 3 ||
        store.operands()[0] != "int") {
      continue;
    }
    const auto addFound = definitions.find(store.operands()[1]);
    if (addFound == definitions.end() ||
        addFound->second.instruction->opcode() != "add" ||
        addFound->second.instruction->operands().size() != 2 ||
        addFound->second.block != block.name()) {
      continue;
    }
    const auto& addOperands = addFound->second.instruction->operands();
    std::string loadValue;
    if (detail::isOne(addOperands[0])) {
      loadValue = addOperands[1];
    } else if (detail::isOne(addOperands[1])) {
      loadValue = addOperands[0];
    } else {
      continue;
    }
    const auto loadFound = definitions.find(loadValue);
    if (loadFound != definitions.end() && loadFound->second.block == block.name() &&
        isLoadFrom(*loadFound->second.instruction, store.operands()[2])) {
      return std::pair{store.operands()[2], store.operands()[1]};
    }
  }
  return std::nullopt;
}

std::optional<std::unordered_set<std::string>> collectLoopRegion(
    const Function& function, const CFG& cfg, const std::string& bodyEntry,
    const std::string& header, const std::string& exit,
    const std::string& latch) {
  std::unordered_set<std::string> region;
  std::vector<std::string> work{bodyEntry};
  while (!work.empty()) {
    const std::string current = std::move(work.back());
    work.pop_back();
    if (current == header || current == exit || !region.insert(current).second) {
      continue;
    }
    const auto found = cfg.index.find(current);
    if (found == cfg.index.end()) return std::nullopt;
    for (const auto& successor : cfg.successors[found->second]) {
      if (successor == exit) return std::nullopt;
      if (successor == header && current != latch) return std::nullopt;
      if (successor != header) work.push_back(successor);
    }
  }
  if (region.find(latch) == region.end()) return std::nullopt;
  for (const auto& name : region) {
    const auto* block = findBlock(function, cfg, name);
    if (block == nullptr || block->terminator() == nullptr) return std::nullopt;
  }
  return region;
}

bool valueUsedBy(const Function& function, const std::string& value,
                 const Instruction* allowedUser) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand == value && &instruction != allowedUser) return true;
      }
    }
  }
  return false;
}

std::optional<std::string> validateReductionRegion(
    const Function& function, const CFG& cfg, const DefinitionMap& definitions,
    const std::unordered_set<std::string>& region, const std::string& latch,
    const std::string& counterSlot, const std::string& count,
    const BasicBlock& preheader) {
  std::unordered_set<std::string> localSlots;
  for (const auto& name : region) {
    const auto* block = findBlock(function, cfg, name);
    for (std::size_t index = 0; index < block->instructions().size(); ++index) {
      const auto& instruction = block->instructions()[index];
      if (instruction.opcode() != "alloca" || !instruction.hasResult()) continue;
      if (index + 1 >= block->instructions().size() ||
          !isStoreTo(block->instructions()[index + 1], instruction.result()) ||
          !detail::isZero(block->instructions()[index + 1].operands()[1])) {
        return std::nullopt;
      }
      localSlots.insert(instruction.result());
    }
  }

  std::unordered_set<std::string> externalStoreSlots;
  const Instruction* reductionLoad = nullptr;
  const Instruction* reductionStore = nullptr;
  const Instruction* reductionAdd = nullptr;
  const Instruction* counterLoad = nullptr;
  const Instruction* counterStore = nullptr;

  for (const auto& name : region) {
    const auto* block = findBlock(function, cfg, name);
    for (const auto& instruction : block->instructions()) {
      if (instruction.opcode() == "call" ||
          std::find(instruction.operands().begin(), instruction.operands().end(),
                    count) != instruction.operands().end()) {
        return std::nullopt;
      }
      if (instruction.opcode() == "store") {
        if (instruction.operands().size() != 3 ||
            instruction.operands()[0] != "int") {
          return std::nullopt;
        }
        const auto& slot = instruction.operands()[2];
        if (slot == counterSlot) {
          if (name != latch || counterStore != nullptr) return std::nullopt;
          counterStore = &instruction;
        } else if (localSlots.find(slot) == localSlots.end()) {
          externalStoreSlots.insert(slot);
        }
      }
    }
  }
  if (externalStoreSlots.size() != 1 || counterStore == nullptr) {
    return std::nullopt;
  }
  const std::string reductionSlot = *externalStoreSlots.begin();
  if (!blockStoresZeroTo(preheader, reductionSlot)) return std::nullopt;

  for (const auto& name : region) {
    const auto* block = findBlock(function, cfg, name);
    for (const auto& instruction : block->instructions()) {
      if (isLoadFrom(instruction, counterSlot)) {
        if (name != latch || counterLoad != nullptr) return std::nullopt;
        counterLoad = &instruction;
      }
      if (isLoadFrom(instruction, reductionSlot)) {
        if (reductionLoad != nullptr) return std::nullopt;
        reductionLoad = &instruction;
      }
      if (isStoreTo(instruction, reductionSlot)) {
        if (reductionStore != nullptr) return std::nullopt;
        reductionStore = &instruction;
      }
    }
  }
  if (counterLoad == nullptr || reductionLoad == nullptr ||
      reductionStore == nullptr) {
    return std::nullopt;
  }

  // A loop counter may feed its unit increment and nothing else in the body.
  // In particular, using it in an address makes the per-iteration reduction
  // delta vary and therefore forbids collapsing the loop.
  const auto counterAddFound = definitions.find(counterStore->operands()[1]);
  if (counterAddFound == definitions.end() ||
      counterAddFound->second.instruction->opcode() != "add" ||
      valueUsedBy(function, counterLoad->result(),
                  counterAddFound->second.instruction) ||
      valueUsedBy(function, counterAddFound->second.instruction->result(),
                  counterStore)) {
    return std::nullopt;
  }

  const auto addFound = definitions.find(reductionStore->operands()[1]);
  if (addFound == definitions.end() ||
      addFound->second.instruction->opcode() != "add" ||
      addFound->second.instruction->operands().size() != 2 ||
      region.find(addFound->second.block) == region.end()) {
    return std::nullopt;
  }
  reductionAdd = addFound->second.instruction;
  const auto& addOperands = reductionAdd->operands();
  if (addOperands[0] != reductionLoad->result() &&
      addOperands[1] != reductionLoad->result()) {
    return std::nullopt;
  }
  if (valueUsedBy(function, reductionLoad->result(), reductionAdd) ||
      valueUsedBy(function, reductionAdd->result(), reductionStore)) {
    return std::nullopt;
  }

  for (const auto& slot : localSlots) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        for (std::size_t index = 0; index < instruction.operands().size(); ++index) {
          if (instruction.operands()[index] != slot) continue;
          const bool legalPointerUse =
              (instruction.opcode() == "load" && index == 1) ||
              (instruction.opcode() == "store" && index == 2);
          if (!legalPointerUse || region.find(block.name()) == region.end()) {
            return std::nullopt;
          }
        }
      }
    }
  }
  return reductionSlot;
}

std::optional<Candidate> findCandidate(
    const Function& function, const std::unordered_set<std::string>& excluded) {
  const CFG cfg = buildCFG(function);
  const DefinitionMap definitions = buildDefinitions(function);
  for (const auto& header : function.blocks()) {
    if (excluded.find(header.name()) != excluded.end()) continue;
    const auto* terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto conditionFound = definitions.find(terminator->operands()[0]);
    if (conditionFound == definitions.end() ||
        conditionFound->second.block != header.name() ||
        conditionFound->second.instruction->opcode() != "icmp.slt" ||
        conditionFound->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto& comparison = *conditionFound->second.instruction;
    const auto counterFound = definitions.find(comparison.operands()[0]);
    if (counterFound == definitions.end() ||
        counterFound->second.block != header.name() ||
        counterFound->second.instruction->opcode() != "load" ||
        counterFound->second.instruction->operands().size() != 2 ||
        counterFound->second.instruction->operands()[0] != "int") {
      continue;
    }
    if (valueUsedBy(function, counterFound->second.instruction->result(),
                    &comparison)) {
      continue;
    }
    const std::string counterSlot =
        counterFound->second.instruction->operands()[1];
    const auto headerIndex = cfg.index.at(header.name());
    if (cfg.predecessors[headerIndex].size() != 2) continue;

    std::optional<std::string> latchName;
    std::optional<std::string> preheaderName;
    for (const auto& predecessor : cfg.predecessors[headerIndex]) {
      const auto* block = findBlock(function, cfg, predecessor);
      const auto increment = unitIncrementSlot(*block, definitions);
      if (increment.has_value() && increment->first == counterSlot) {
        if (latchName.has_value()) {
          latchName.reset();
          break;
        }
        latchName = predecessor;
      } else {
        if (preheaderName.has_value()) {
          preheaderName.reset();
          break;
        }
        preheaderName = predecessor;
      }
    }
    if (!latchName.has_value() || !preheaderName.has_value()) continue;
    const auto* preheader = findBlock(function, cfg, *preheaderName);
    if (preheader == nullptr || branchTargets(*preheader) !=
                                    std::vector<std::string>{header.name()} ||
        !blockStoresZeroTo(*preheader, counterSlot)) {
      continue;
    }

    const std::string bodyEntry = terminator->operands()[1];
    const std::string exit = terminator->operands()[2];
    const auto region = collectLoopRegion(function, cfg, bodyEntry, header.name(),
                                          exit, *latchName);
    if (!region.has_value()) continue;
    const auto reductionSlot = validateReductionRegion(
        function, cfg, definitions, *region, *latchName, counterSlot,
        comparison.operands()[1], *preheader);
    if (!reductionSlot.has_value()) continue;

    return Candidate{header.name(), bodyEntry, exit, *latchName,
                     *preheaderName, counterSlot, *reductionSlot,
                     counterFound->second.instruction->result(),
                     terminator->operands()[0], comparison.operands()[1]};
  }
  return std::nullopt;
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "%" + stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

bool transform(Function& function, const Candidate& candidate) {
  auto* header = findBlock(function, candidate.header);
  auto* exit = findBlock(function, candidate.exit);
  if (header == nullptr || exit == nullptr || header->instructions().empty()) {
    return false;
  }
  const std::string firstIteration =
      freshValueName(function, "nested.reduce.first");
  const std::string executeOnce =
      freshValueName(function, "nested.reduce.execute");
  const std::string reductionValue =
      freshValueName(function, "nested.reduce.value");
  const std::string scaledValue =
      freshValueName(function, "nested.reduce.scaled");

  auto& headerInstructions = header->instructions();
  headerInstructions.insert(
      headerInstructions.end() - 1,
      Instruction{"icmp.eq", {candidate.counterValue, "0"}, firstIteration});
  headerInstructions.insert(
      headerInstructions.end() - 1,
      Instruction{"and", {candidate.originalCondition, firstIteration},
                  executeOnce});
  headerInstructions.back() =
      Instruction{"br", {executeOnce, candidate.bodyEntry, candidate.exit}};

  auto insert = exit->instructions().begin();
  while (insert != exit->instructions().end() && insert->opcode() == "phi") {
    ++insert;
  }
  insert = exit->instructions().insert(
      insert, Instruction{"load", {"int", candidate.reductionSlot},
                          reductionValue});
  ++insert;
  insert = exit->instructions().insert(
      insert, Instruction{"mul", {reductionValue, candidate.count}, scaledValue});
  ++insert;
  exit->instructions().insert(
      insert, Instruction{"store", {"int", scaledValue,
                                     candidate.reductionSlot}});
  return true;
}

}  // namespace

void NestedInvariantReductionPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    std::unordered_set<std::string> transformedHeaders;
    while (const auto candidate = findCandidate(function, transformedHeaders)) {
      transformedHeaders.insert(candidate->header);
      if (!transform(function, *candidate)) break;
    }
  }
}

}  // namespace compiler::midend::pass
