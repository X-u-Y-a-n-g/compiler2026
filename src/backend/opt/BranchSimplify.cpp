#include "../../../include/compiler/backend/opt/BranchSimplify.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperandKind;

bool isTrivialJumpBlock(
    const MachineBlock& block,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  return block.instructions.size() == 1 && adapter.isUnconditionalBranch(block.instructions.front());
}

std::string resolveTarget(const std::string& target,
                          const std::unordered_map<std::string, std::string>& forwarding) {
  std::string current = target;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    const auto found = forwarding.find(current);
    if (found == forwarding.end() || found->second == current) {
      return current;
    }
    current = found->second;
  }
  return target;
}

void rewriteBranchTargets(compiler::backend::mir::MachineFunction& function,
                          const std::unordered_map<std::string, std::string>& forwarding) {
  for (auto& block : function.blocks) {
    for (auto& instruction : block.instructions) {
      if (instruction.opcode != "br") {
        continue;
      }
      for (auto& operand : instruction.operands) {
        if (operand.kind == MachineOperandKind::Label) {
          operand.text = resolveTarget(operand.text, forwarding);
        }
      }
    }
  }
}

}  // namespace

void BranchSimplify::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    if (function.blocks.empty()) {
      continue;
    }

    std::unordered_map<std::string, std::string> forwarding;
    for (const auto& block : function.blocks) {
      if (!isTrivialJumpBlock(block, adapter)) {
        continue;
      }
      const auto targets = adapter.branchTargets(block.instructions.front());
      if (targets.size() == 1 && targets.front() != block.name) {
        forwarding.emplace(block.name, targets.front());
      }
    }

    if (!forwarding.empty()) {
      rewriteBranchTargets(function, forwarding);
    }

    const auto reachable = detail::reachableBlocks(function, adapter);
    std::unordered_set<std::size_t> keep(reachable.begin(), reachable.end());
    std::vector<MachineBlock> newBlocks;
    newBlocks.reserve(function.blocks.size());
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
      if (keep.find(i) != keep.end()) {
        newBlocks.push_back(std::move(function.blocks[i]));
      }
    }
    function.blocks = std::move(newBlocks);

    for (auto& block : function.blocks) {
      std::vector<compiler::backend::mir::MachineInstruction> kept;
      kept.reserve(block.instructions.size());
      for (auto instruction : block.instructions) {
        if (instruction.kind == MachineInstructionKind::Phi && instruction.phiInputs.empty()) {
          continue;
        }
        if (adapter.isConditionalBranch(instruction) && instruction.operands.size() == 3 &&
            instruction.operands[1].kind == MachineOperandKind::Label &&
            instruction.operands[2].kind == MachineOperandKind::Label &&
            instruction.operands[1].text == instruction.operands[2].text) {
          instruction = compiler::backend::mir::MachineInstruction::target(
              "br", std::vector<compiler::backend::mir::MachineOperand>{instruction.operands[1]});
        }
        kept.push_back(std::move(instruction));
      }
      block.instructions = std::move(kept);
    }

    for (std::size_t i = 0; i + 1 < function.blocks.size(); ++i) {
      auto& block = function.blocks[i];
      if (block.instructions.empty()) {
        continue;
      }
      const auto& terminator = block.instructions.back();
      if (adapter.isUnconditionalBranch(terminator) &&
          adapter.branchTargets(terminator).size() == 1 &&
          adapter.branchTargets(terminator).front() == function.blocks[i + 1].name) {
        block.instructions.pop_back();
      }
    }
  }
}

}  // namespace compiler::backend::opt
