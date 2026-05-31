#include "../../../include/compiler/backend/opt/PhiElimination.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::common::CFGBuilder;
using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

struct CopyPair {
  MachineOperand destination;
  MachineOperand source;
};

std::size_t insertionPoint(const MachineBlock& block) {
  std::size_t index = block.instructions.size();
  while (index > 0) {
    const auto& instruction = block.instructions[index - 1];
    if (instruction.opcode == "br" || instruction.opcode == "ret") {
      --index;
      continue;
    }
    break;
  }
  return index;
}

bool sameOperand(const MachineOperand& lhs, const MachineOperand& rhs) {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  switch (lhs.kind) {
    case MachineOperandKind::VirtualRegister:
    case MachineOperandKind::PhysicalRegister:
    case MachineOperandKind::GlobalAddress:
    case MachineOperandKind::FunctionSymbol:
    case MachineOperandKind::Label:
      return lhs.text == rhs.text;
    case MachineOperandKind::ImmediateInt:
      return lhs.intValue == rhs.intValue;
    case MachineOperandKind::ImmediateFloat:
      return lhs.floatValue == rhs.floatValue;
    case MachineOperandKind::FrameIndex:
    case MachineOperandKind::StackSlot:
    case MachineOperandKind::StackObject:
      return lhs.index == rhs.index;
    case MachineOperandKind::None:
      return true;
  }
  return false;
}

bool isRegisterLike(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister ||
         operand.kind == MachineOperandKind::PhysicalRegister;
}

std::vector<MachineInstruction> lowerParallelCopies(std::vector<CopyPair> copies,
                                                    std::size_t& nextTemporary) {
  std::vector<MachineInstruction> lowered;
  std::vector<CopyPair> pending;
  pending.reserve(copies.size());
  for (auto& copy : copies) {
    if (!sameOperand(copy.destination, copy.source)) {
      pending.push_back(std::move(copy));
    }
  }

  while (!pending.empty()) {
    bool madeProgress = false;
    for (auto it = pending.begin(); it != pending.end(); ++it) {
      bool destinationIsUsed = false;
      for (const auto& copy : pending) {
        if (sameOperand(copy.source, it->destination)) {
          destinationIsUsed = true;
          break;
        }
      }
      if (destinationIsUsed) {
        continue;
      }

      lowered.push_back(MachineInstruction::copy(it->destination, it->source));
      pending.erase(it);
      madeProgress = true;
      break;
    }
    if (madeProgress) {
      continue;
    }

    auto& cycle = pending.front();
    if (!isRegisterLike(cycle.source)) {
      lowered.push_back(MachineInstruction::copy(cycle.destination, cycle.source));
      pending.erase(pending.begin());
      continue;
    }

    const std::string temporaryName = "%phi.copy.tmp" + std::to_string(nextTemporary++);
    const auto originalSource = cycle.source;
    auto temporary = MachineOperand::virtualRegister(temporaryName, originalSource.type);
    lowered.push_back(MachineInstruction::copy(temporary, originalSource));
    for (auto& copy : pending) {
      if (sameOperand(copy.source, originalSource)) {
        copy.source = temporary;
      }
    }
  }

  return lowered;
}

std::vector<std::string> branchTargets(const MachineBlock& block,
                                       const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::vector<std::string> targets;
  std::unordered_set<std::string> seen;
  for (const auto& instruction : block.instructions) {
    for (const auto& target : adapter.branchTargets(instruction)) {
      if (seen.insert(target).second) {
        targets.push_back(target);
      }
    }
  }
  return targets;
}

bool canInsertCopiesInPredecessor(const MachineBlock& predecessor, const std::string& target,
                                  const compiler::backend::target::TargetInstrAdapter& adapter) {
  const auto targets = branchTargets(predecessor, adapter);
  return targets.size() == 1 && targets.front() == target;
}

void replaceBranchTarget(MachineBlock& block, const std::string& from, const std::string& to) {
  for (auto& instruction : block.instructions) {
    if (instruction.opcode != "br") {
      continue;
    }
    for (auto& operand : instruction.operands) {
      if (operand.kind == MachineOperandKind::Label && operand.text == from) {
        operand.text = to;
      }
    }
  }
}

}  // namespace

void PhiElimination::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    std::unordered_map<std::string, std::size_t> blockIndexByName;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
      blockIndexByName.emplace(function.blocks[i].name, i);
    }

    const auto cfg = CFGBuilder{}.build(function, adapter);
    (void)cfg;

    std::unordered_set<std::string> usedBlockNames;
    for (const auto& block : function.blocks) {
      usedBlockNames.insert(block.name);
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<CopyPair>>>
        edgeCopies;

    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
      auto& block = function.blocks[blockIndex];
      std::vector<MachineInstruction> remaining;
      remaining.reserve(block.instructions.size());

      for (const auto& instruction : block.instructions) {
        if (instruction.kind != MachineInstructionKind::Phi) {
          remaining.push_back(instruction);
          continue;
        }

        for (const auto& incoming : instruction.phiInputs) {
          edgeCopies[incoming.block][block.name].push_back(
              CopyPair{instruction.result, incoming.value});
        }
      }

      block.instructions = std::move(remaining);
    }

    std::vector<MachineBlock> bridgeBlocks;
    std::size_t nextTemporary = 0;
    std::size_t nextBridge = 0;

    for (auto& [predecessorName, targets] : edgeCopies) {
      const auto predIt = blockIndexByName.find(predecessorName);
      if (predIt == blockIndexByName.end()) {
        continue;
      }

      for (auto& [targetName, copies] : targets) {
        const auto targetIt = blockIndexByName.find(targetName);
        if (targetIt == blockIndexByName.end()) {
          continue;
        }

        auto lowered = lowerParallelCopies(std::move(copies), nextTemporary);
        if (lowered.empty()) {
          continue;
        }

        auto& predecessor = function.blocks[predIt->second];
        if (canInsertCopiesInPredecessor(predecessor, targetName, adapter)) {
          const std::size_t insertAt = insertionPoint(predecessor);
          predecessor.instructions.insert(predecessor.instructions.begin() + insertAt,
                                          lowered.begin(), lowered.end());
          continue;
        }

        std::string bridgeName;
        do {
          bridgeName = predecessorName + ".phi." + std::to_string(nextBridge++);
        } while (!usedBlockNames.insert(bridgeName).second);

        replaceBranchTarget(predecessor, targetName, bridgeName);

        MachineBlock bridge;
        bridge.name = bridgeName;
        bridge.instructions = std::move(lowered);
        bridge.instructions.push_back(
            MachineInstruction::target("br", std::vector<MachineOperand>{MachineOperand::label(targetName)}));
        bridgeBlocks.push_back(std::move(bridge));
      }
    }

    for (auto& bridge : bridgeBlocks) {
      function.blocks.push_back(std::move(bridge));
    }
  }
}

}  // namespace compiler::backend::opt
