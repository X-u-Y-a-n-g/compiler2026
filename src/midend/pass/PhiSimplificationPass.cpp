#include "../../../include/compiler/midend/pass/PhiSimplificationPass.h"

#include <algorithm>
#include <deque>
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

bool isValueReference(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

std::string zeroValueForType(const std::string &type) {
  return type == "float" ? "0.0" : "0";
}

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto &operands = terminator->operands();
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

std::unordered_map<std::string, std::unordered_set<std::string>>
predecessorMap(const Function &function) {
  std::unordered_map<std::string, std::unordered_set<std::string>> predecessors;
  for (const auto &block : function.blocks()) {
    predecessors.emplace(block.name(), std::unordered_set<std::string>{});
  }
  for (const auto &block : function.blocks()) {
    for (const auto &target : branchTargets(block)) {
      predecessors[target].insert(block.name());
    }
  }
  return predecessors;
}

void rewriteFunctionOperands(
    Function &function,
    const std::unordered_map<std::string, std::string> &aliases) {
  if (aliases.empty()) {
    return;
  }

  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      rewritten.emplace_back(
          instruction.opcode(),
          detail::rewriteOperands(instruction.operands(), aliases),
          instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

bool prunePhiIncoming(Function &function) {
  const auto predecessors = predecessorMap(function);
  bool changed = false;

  for (auto &block : function.blocks()) {
    const auto predIt = predecessors.find(block.name());
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }

      std::vector<std::string> operands;
      const auto &oldOperands = instruction.operands();
      if (!oldOperands.empty()) {
        operands.push_back(oldOperands.front());
      }
      std::unordered_map<std::string, std::string> valueByPred;
      for (std::size_t i = 1; i + 1 < oldOperands.size(); i += 2) {
        const auto &value = oldOperands[i];
        const auto &pred = oldOperands[i + 1];
        if (predIt == predecessors.end() ||
            predIt->second.find(pred) == predIt->second.end()) {
          continue;
        }

        const auto existing = valueByPred.find(pred);
        if (existing != valueByPred.end() && existing->second == value) {
          changed = true;
          continue;
        }
        valueByPred.emplace(pred, value);
        operands.push_back(value);
        operands.push_back(pred);
      }

      if (operands.size() != oldOperands.size()) {
        changed = true;
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  return changed;
}

bool simplifyTrivialPhis(Function &function) {
  const auto predecessors = predecessorMap(function);
  std::unordered_map<std::string, std::string> aliases;
  bool changed = false;

  for (auto &block : function.blocks()) {
    const auto predIt = predecessors.find(block.name());
    const std::size_t expectedIncomingCount =
        predIt == predecessors.end() ? 0 : predIt->second.size();
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi" || !instruction.hasResult()) {
        rewritten.emplace_back(
            instruction.opcode(),
            detail::rewriteOperands(instruction.operands(), aliases),
            instruction.result());
        continue;
      }

      std::vector<std::string> operands =
          detail::rewriteOperands(instruction.operands(), aliases);
      std::string commonValue;
      bool hasIncoming = false;
      bool allSame = true;
      std::size_t incomingCount = 0;
      for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
        ++incomingCount;
        const auto &value = operands[i];
        if (!hasIncoming) {
          commonValue = value;
          hasIncoming = true;
        } else if (value != commonValue) {
          allSame = false;
          break;
        }
      }

      if (incomingCount != expectedIncomingCount) {
        rewritten.emplace_back(instruction.opcode(), std::move(operands),
                               instruction.result());
        continue;
      }

      if (!hasIncoming || (allSame && commonValue != instruction.result())) {
        aliases[instruction.result()] =
            hasIncoming
                ? commonValue
                : (operands.empty() ? "0" : zeroValueForType(operands.front()));
        changed = true;
        continue;
      }

      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  rewriteFunctionOperands(function, aliases);
  return changed;
}

std::vector<std::string> phiUsedValues(const Instruction &instruction) {
  std::vector<std::string> uses;
  const auto &operands = instruction.operands();
  for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
    if (isValueReference(operands[i])) {
      uses.push_back(operands[i]);
    }
  }
  return uses;
}

std::vector<std::string> nonPhiUsedValues(const Instruction &instruction) {
  std::vector<std::string> uses;
  const auto &operands = instruction.operands();
  if (instruction.opcode() == "br") {
    if (operands.size() == 3 && isValueReference(operands[0])) {
      uses.push_back(operands[0]);
    }
    return uses;
  }
  if (instruction.opcode() == "ret") {
    for (const auto &operand : operands) {
      if (isValueReference(operand)) {
        uses.push_back(operand);
      }
    }
    return uses;
  }
  for (const auto &operand : operands) {
    if (isValueReference(operand)) {
      uses.push_back(operand);
    }
  }
  return uses;
}

bool removeDeadPhiCycles(Function &function) {
  std::unordered_set<std::string> phiResults;
  std::unordered_map<std::string, std::vector<std::string>> phiDependencies;
  std::deque<std::string> worklist;
  std::unordered_set<std::string> livePhis;

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "phi" && instruction.hasResult()) {
        phiResults.insert(instruction.result());
      }
    }
  }
  if (phiResults.empty()) {
    return false;
  }

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "phi" && instruction.hasResult()) {
        for (const auto &use : phiUsedValues(instruction)) {
          if (phiResults.find(use) != phiResults.end()) {
            phiDependencies[instruction.result()].push_back(use);
          }
        }
        continue;
      }

      for (const auto &use : nonPhiUsedValues(instruction)) {
        if (phiResults.find(use) != phiResults.end() &&
            livePhis.insert(use).second) {
          worklist.push_back(use);
        }
      }
    }
  }

  while (!worklist.empty()) {
    const auto value = worklist.front();
    worklist.pop_front();
    const auto deps = phiDependencies.find(value);
    if (deps == phiDependencies.end()) {
      continue;
    }
    for (const auto &dependency : deps->second) {
      if (livePhis.insert(dependency).second) {
        worklist.push_back(dependency);
      }
    }
  }

  bool changed = false;
  for (auto &block : function.blocks()) {
    std::vector<Instruction> kept;
    kept.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "phi" && instruction.hasResult() &&
          livePhis.find(instruction.result()) == livePhis.end()) {
        changed = true;
        continue;
      }
      kept.push_back(instruction);
    }
    block.instructions() = std::move(kept);
  }
  return changed;
}

void simplifyFunction(Function &function) {
  bool changed = false;
  do {
    changed = false;
    changed |= prunePhiIncoming(function);
    changed |= simplifyTrivialPhis(function);
    changed |= removeDeadPhiCycles(function);
  } while (changed);
}

} // namespace

bool PhiSimplificationPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    simplifyFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
