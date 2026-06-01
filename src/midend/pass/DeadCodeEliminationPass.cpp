#include "../../../include/compiler/midend/pass/DeadCodeEliminationPass.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

bool isValueReference(const std::string& operand) {
  return !operand.empty() && (operand.front() == '%' || operand.front() == '@');
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

bool removeUnreachableBlocks(Function& function) {
  const auto reachable = reachableBlocks(function);
  const auto oldSize = function.blocks().size();
  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock& block) {
                       return reachable.find(block.name()) == reachable.end();
                     }),
      function.blocks().end());

  if (oldSize == function.blocks().size()) {
    return false;
  }

  const auto predecessors = predecessorMap(function);
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
      if (!instruction.operands().empty()) {
        operands.push_back(instruction.operands().front());
      }
      for (std::size_t i = 2; i < instruction.operands().size(); i += 2) {
        if (predIt != predecessors.end() &&
            predIt->second.find(instruction.operands()[i]) != predIt->second.end()) {
          operands.push_back(instruction.operands()[i - 1]);
          operands.push_back(instruction.operands()[i]);
        }
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  return true;
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
  return opcode == "br" || opcode == "ret" || opcode == "store" || opcode == "call";
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
      if (!instruction.hasResult() || live.find(&instruction) != live.end()) {
        kept.push_back(instruction);
      } else {
        changed = true;
      }
    }
    block.instructions() = std::move(kept);
  }

  return changed;
}

void runDce(Function& function) {
  removeUnreachableBlocks(function);
  eliminateDeadInstructions(function);
}

}  // namespace

void DeadCodeEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    runDce(function);
  }
}

}  // namespace compiler::midend::pass
