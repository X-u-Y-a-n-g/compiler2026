#include "../../../include/compiler/midend/pass/DeadCodeEliminationPass.h"

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

bool isValueReference(const std::string& operand) {
  return !operand.empty() && (operand.front() == '%' || operand.front() == '@');
}

bool isTrueConstant(const std::string& operand) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value != 0;
}

bool isFalseConstant(const std::string& operand) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value == 0;
}

std::string zeroValueForType(const std::string& type) {
  return type == "float" ? "0.0" : "0";
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

  std::unordered_map<std::string, const BasicBlock*> blockByName;
  for (const auto& block : function.blocks()) {
    blockByName.emplace(block.name(), &block);
  }

  std::deque<std::string> worklist;
  reachable.insert(function.blocks().front().name());
  worklist.push_back(function.blocks().front().name());
  while (!worklist.empty()) {
    const auto name = worklist.front();
    worklist.pop_front();
    const auto found = blockByName.find(name);
    if (found == blockByName.end()) {
      continue;
    }
    for (const auto& target : branchTargets(*found->second)) {
      if (blockByName.find(target) != blockByName.end() && reachable.insert(target).second) {
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
    for (const auto& target : branchTargets(block)) {
      predecessors[target].insert(block.name());
    }
  }
  return predecessors;
}

bool simplifyConstantBranches(Function& function) {
  bool changed = false;
  for (auto& block : function.blocks()) {
    auto& instructions = block.instructions();
    if (instructions.empty()) {
      continue;
    }
    const auto& terminator = instructions.back();
    if (terminator.opcode() != "br" || terminator.operands().size() != 3) {
      continue;
    }

    const auto& operands = terminator.operands();
    std::string target;
    if (operands[1] == operands[2]) {
      target = operands[1];
    } else if (isTrueConstant(operands[0])) {
      target = operands[1];
    } else if (isFalseConstant(operands[0])) {
      target = operands[2];
    } else {
      continue;
    }

    instructions.back() = Instruction{"br", {std::move(target)}};
    changed = true;
  }
  return changed;
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
        const auto& value = oldOperands[i];
        const auto& pred = oldOperands[i + 1];
        if (predIt != predecessors.end() && predIt->second.find(pred) != predIt->second.end()) {
          operands.push_back(value);
          operands.push_back(pred);
        }
      }

      if (operands.size() != oldOperands.size()) {
        changed = true;
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  return changed;
}

void rewriteFunctionOperands(
    Function& function, const std::unordered_map<std::string, std::string>& aliases) {
  if (aliases.empty()) {
    return;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(), aliases),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

bool simplifyPhis(Function& function) {
  std::unordered_map<std::string, std::string> aliases;
  bool changed = false;

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi" || !instruction.hasResult()) {
        rewritten.emplace_back(instruction.opcode(),
                               detail::rewriteOperands(instruction.operands(), aliases),
                               instruction.result());
        continue;
      }

      std::vector<std::string> operands =
          detail::rewriteOperands(instruction.operands(), aliases);
      std::string commonValue;
      bool hasIncoming = false;
      bool allSame = true;
      for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
        const auto& value = operands[i];
        if (!hasIncoming) {
          commonValue = value;
          hasIncoming = true;
        } else if (value != commonValue) {
          allSame = false;
          break;
        }
      }

      if (!hasIncoming || allSame) {
        aliases[instruction.result()] =
            hasIncoming ? commonValue
                        : (operands.empty() ? "0" : zeroValueForType(operands.front()));
        changed = true;
        continue;
      }

      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  rewriteFunctionOperands(function, aliases);
  return changed;
}

std::vector<std::string> usedValues(const Instruction& instruction) {
  std::vector<std::string> uses;
  const auto& operands = instruction.operands();

  if (instruction.opcode() == "phi") {
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      if (isValueReference(operands[i])) {
        uses.push_back(operands[i]);
      }
    }
    return uses;
  }

  if (instruction.opcode() == "br") {
    if (operands.size() == 3 && isValueReference(operands[0])) {
      uses.push_back(operands[0]);
    }
    return uses;
  }

  if (instruction.opcode() == "ret") {
    for (const auto& operand : operands) {
      if (isValueReference(operand)) {
        uses.push_back(operand);
      }
    }
    return uses;
  }

  for (const auto& operand : operands) {
    if (isValueReference(operand)) {
      uses.push_back(operand);
    }
  }
  return uses;
}

bool isEssential(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  return opcode == "br" || opcode == "ret" || opcode == "store" ||
         opcode == "call" || opcode == "aarch64.neon.dup.i32x4" ||
         opcode == "aarch64.neon.mls.i32x4" ||
         opcode == "aarch64.neon.load.acc.i32x4" ||
         opcode == "aarch64.neon.mls.acc.i32x4" ||
         opcode == "aarch64.neon.store.acc.i32x4" ||
         opcode == "aarch64.neon.load.acc.i32x8" ||
         opcode == "aarch64.neon.mls.acc.i32x8" ||
         opcode == "aarch64.neon.store.acc.i32x8" ||
         opcode == "aarch64.neon.zero.acc.i32x32" ||
         opcode == "aarch64.neon.dup.acc.i32x32" ||
         opcode == "aarch64.neon.mla.acc.i32x32" ||
         opcode == "aarch64.neon.store.acc.i32x32" ||
         opcode == "aarch64.widened.mul.lhs.i32" ||
         opcode == "aarch64.widened.mul.rhs.i32" ||
         opcode == "aarch64.widened.mod.i32";
}

bool eliminateDeadInstructions(Function& function) {
  std::unordered_map<std::string, const Instruction*> definitions;
  std::vector<const Instruction*> roots;

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions[instruction.result()] = &instruction;
      }
      if (isEssential(instruction)) {
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
    for (const auto& operand : usedValues(*instruction)) {
      const auto found = definitions.find(operand);
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

void runDce(Function& function) {
  bool changed = false;
  do {
    changed = false;
    changed |= simplifyConstantBranches(function);
    changed |= removeUnreachableBlocks(function);
    changed |= prunePhiIncoming(function);
    changed |= simplifyPhis(function);
    changed |= eliminateDeadInstructions(function);
  } while (changed);
}

}  // namespace

void DeadCodeEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    runDce(function);
  }
}

}  // namespace compiler::midend::pass
