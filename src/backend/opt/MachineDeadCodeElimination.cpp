#include "../../../include/compiler/backend/opt/MachineDeadCodeElimination.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "../../../include/compiler/backend/target/TargetInstrAdapter.h"

namespace compiler::backend::opt {

namespace {

using RegisterSet = std::unordered_set<std::string>;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::target::TargetInstrAdapter;

void addPhysicalRegisters(const std::vector<MachineOperand>& operands,
                          RegisterSet& registers) {
  for (const auto& operand : operands) {
    if (operand.kind == MachineOperandKind::PhysicalRegister) {
      registers.insert(operand.text);
    }
  }
}

bool isRemovableDefinition(const MachineInstruction& instruction,
                           const TargetInstrAdapter& adapter) {
  if (!instruction.hasResult ||
      instruction.result.kind != MachineOperandKind::PhysicalRegister) {
    return false;
  }
  return !adapter.isCall(instruction) && !adapter.isReturn(instruction) &&
         !adapter.isConditionalBranch(instruction) &&
         !adapter.isUnconditionalBranch(instruction);
}

void eliminateDeadDefinitions(MachineFunction& function,
                              const TargetInstrAdapter& adapter) {
  const auto cfg = compiler::backend::common::CFGBuilder{}.build(function, adapter);
  const std::size_t blockCount = function.blocks.size();
  std::vector<RegisterSet> blockUses(blockCount);
  std::vector<RegisterSet> blockDefs(blockCount);
  std::vector<MachineOperand> uses;
  std::vector<MachineOperand> defs;

  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    for (const auto& instruction : function.blocks[blockIndex].instructions) {
      adapter.enumerateUses(instruction, uses);
      for (const auto& operand : uses) {
        if (operand.kind == MachineOperandKind::PhysicalRegister &&
            blockDefs[blockIndex].find(operand.text) ==
                blockDefs[blockIndex].end()) {
          blockUses[blockIndex].insert(operand.text);
        }
      }
      adapter.enumerateDefs(instruction, defs);
      addPhysicalRegisters(defs, blockDefs[blockIndex]);
    }
  }

  std::vector<RegisterSet> liveIn(blockCount);
  std::vector<RegisterSet> liveOut(blockCount);
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t reverseIndex = blockCount; reverseIndex > 0; --reverseIndex) {
      const std::size_t blockIndex = reverseIndex - 1;
      RegisterSet nextOut;
      for (const auto successor : cfg.successors[blockIndex]) {
        nextOut.insert(liveIn[successor].begin(), liveIn[successor].end());
      }
      RegisterSet nextIn = blockUses[blockIndex];
      for (const auto& name : nextOut) {
        if (blockDefs[blockIndex].find(name) == blockDefs[blockIndex].end()) {
          nextIn.insert(name);
        }
      }
      if (nextOut != liveOut[blockIndex] || nextIn != liveIn[blockIndex]) {
        liveOut[blockIndex] = std::move(nextOut);
        liveIn[blockIndex] = std::move(nextIn);
        changed = true;
      }
    }
  }

  for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
    RegisterSet live = liveOut[blockIndex];
    auto& instructions = function.blocks[blockIndex].instructions;
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(instructions.size());

    for (auto iterator = instructions.rbegin(); iterator != instructions.rend();
         ++iterator) {
      adapter.enumerateDefs(*iterator, defs);
      bool hasPhysicalDefinition = false;
      bool allDefinitionsDead = true;
      for (const auto& operand : defs) {
        if (operand.kind != MachineOperandKind::PhysicalRegister) {
          allDefinitionsDead = false;
          continue;
        }
        hasPhysicalDefinition = true;
        if (live.find(operand.text) != live.end()) {
          allDefinitionsDead = false;
        }
      }

      if (hasPhysicalDefinition && allDefinitionsDead &&
          isRemovableDefinition(*iterator, adapter)) {
        continue;
      }

      for (const auto& operand : defs) {
        if (operand.kind == MachineOperandKind::PhysicalRegister) {
          live.erase(operand.text);
        }
      }
      adapter.enumerateUses(*iterator, uses);
      addPhysicalRegisters(uses, live);
      rewritten.push_back(*iterator);
    }
    std::reverse(rewritten.begin(), rewritten.end());
    instructions = std::move(rewritten);
  }
}

}  // namespace

void MachineDeadCodeElimination::run(
    compiler::backend::mir::MachineModule& module) const {
  const auto& adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  for (auto& function : module.functions) {
    eliminateDeadDefinitions(function, adapter);
  }
}

}  // namespace compiler::backend::opt
