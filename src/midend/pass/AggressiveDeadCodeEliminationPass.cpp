#include "../../../include/compiler/midend/pass/AggressiveDeadCodeEliminationPass.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

bool isLocalValue(const std::string& operand) {
  return !operand.empty() && operand.front() == '%';
}

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto& operands = terminator->operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

std::unordered_set<std::string> reachableBlocks(const Function& function) {
  std::unordered_set<std::string> reachable;
  if (function.blocks().empty()) {
    return reachable;
  }

  std::unordered_map<std::string, const BasicBlock*> blocks;
  for (const auto& block : function.blocks()) {
    blocks.emplace(block.name(), &block);
  }

  std::deque<std::string> worklist;
  reachable.insert(function.blocks().front().name());
  worklist.push_back(function.blocks().front().name());
  while (!worklist.empty()) {
    const auto blockName = worklist.front();
    worklist.pop_front();
    const auto block = blocks.find(blockName);
    if (block == blocks.end()) {
      continue;
    }
    for (const auto& target : branchTargets(*block->second)) {
      if (blocks.find(target) != blocks.end() && reachable.insert(target).second) {
        worklist.push_back(target);
      }
    }
  }
  return reachable;
}

std::unordered_map<std::string, std::unordered_set<std::string>> predecessorMap(
    const Function& function) {
  std::unordered_map<std::string, std::unordered_set<std::string>> predecessors;
  for (const auto& block : function.blocks()) {
    predecessors.emplace(block.name(), std::unordered_set<std::string>{});
  }
  for (const auto& block : function.blocks()) {
    for (const auto& target : branchTargets(block)) {
      predecessors[target].insert(block.name());
    }
  }
  return predecessors;
}

std::vector<std::string> usedValues(const Instruction& instruction) {
  std::vector<std::string> uses;
  const auto& operands = instruction.operands();
  if (instruction.opcode() == "phi") {
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      if (isLocalValue(operands[i])) {
        uses.push_back(operands[i]);
      }
    }
    return uses;
  }
  if (instruction.opcode() == "br") {
    if (operands.size() == 3 && isLocalValue(operands[0])) {
      uses.push_back(operands[0]);
    }
    return uses;
  }
  for (const auto& operand : operands) {
    if (isLocalValue(operand)) {
      uses.push_back(operand);
    }
  }
  return uses;
}

bool isLiveRoot(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  return opcode == "ret" || opcode == "br" || opcode == "store" || opcode == "call";
}

bool removeUnreachableBlocks(Function& function) {
  const auto reachable = reachableBlocks(function);
  const auto oldSize = function.blocks().size();
  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock& block) {
                       return reachable.find(block.name()) == reachable.end();
                     }),
      function.blocks().end());
  return oldSize != function.blocks().size();
}

bool prunePhiIncoming(Function& function) {
  const auto predecessors = predecessorMap(function);
  bool changed = false;
  for (auto& block : function.blocks()) {
    const auto predIt = predecessors.find(block.name());
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }

      std::vector<std::string> operands;
      const auto& oldOperands = instruction.operands();
      if (!oldOperands.empty()) {
        operands.push_back(oldOperands.front());
      }
      for (std::size_t i = 1; i + 1 < oldOperands.size(); i += 2) {
        if (predIt != predecessors.end() &&
            predIt->second.find(oldOperands[i + 1]) != predIt->second.end()) {
          operands.push_back(oldOperands[i]);
          operands.push_back(oldOperands[i + 1]);
        }
      }
      changed |= operands.size() != oldOperands.size();
      rewritten.emplace_back("phi", std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
  return changed;
}

bool eliminateDeadDefinitions(Function& function) {
  std::unordered_map<std::string, const Instruction*> definitions;
  std::vector<const Instruction*> roots;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions[instruction.result()] = &instruction;
      }
      if (isLiveRoot(instruction)) {
        roots.push_back(&instruction);
      }
    }
  }

  std::unordered_set<const Instruction*> live;
  std::deque<const Instruction*> worklist;
  for (const auto* root : roots) {
    if (live.insert(root).second) {
      worklist.push_back(root);
    }
  }
  while (!worklist.empty()) {
    const auto* instruction = worklist.front();
    worklist.pop_front();
    for (const auto& use : usedValues(*instruction)) {
      const auto found = definitions.find(use);
      if (found != definitions.end() && live.insert(found->second).second) {
        worklist.push_back(found->second);
      }
    }
  }

  bool changed = false;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> kept;
    kept.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && detail::isSideEffectFree(instruction) &&
          live.find(&instruction) == live.end()) {
        changed = true;
        continue;
      }
      kept.push_back(instruction);
    }
    block.instructions() = std::move(kept);
  }
  return changed;
}

bool runAdce(Function& function) {
  bool changed = false;
  bool localChanged = false;
  do {
    localChanged = false;
    localChanged |= removeUnreachableBlocks(function);
    localChanged |= prunePhiIncoming(function);
    localChanged |= eliminateDeadDefinitions(function);
    changed |= localChanged;
  } while (localChanged);
  return changed;
}

}  // namespace

bool AggressiveDeadCodeEliminationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runAdce(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
