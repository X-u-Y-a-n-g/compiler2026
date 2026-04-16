#include "compiler/backend/opt/StackObjectPromotion.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::mir::ObjectType;
using compiler::backend::mir::StackObjectKind;
using compiler::backend::mir::ValueType;

bool isPromotableObject(const compiler::backend::mir::StackObject& object) {
  return object.kind == StackObjectKind::Local && object.entryAllocated &&
         !object.objectType.isArray() &&
         object.objectType.isIntegerScalar();
}

void markInvalidIfUsedOutsideDirectAccess(const MachineInstruction& instruction,
                                          std::vector<bool>& promotable) {
  const auto invalidate = [&](const MachineOperand& operand) {
    if (operand.kind == MachineOperandKind::FrameIndex && operand.index >= 0 &&
        static_cast<std::size_t>(operand.index) < promotable.size()) {
      promotable[static_cast<std::size_t>(operand.index)] = false;
    }
  };

  if (instruction.opcode == "load") {
    for (std::size_t i = 1; i < instruction.operands.size(); ++i) {
      invalidate(instruction.operands[i]);
    }
    return;
  }

  if (instruction.opcode == "store") {
    if (!instruction.operands.empty()) {
      invalidate(instruction.operands[0]);
    }
    for (std::size_t i = 2; i < instruction.operands.size(); ++i) {
      invalidate(instruction.operands[i]);
    }
    return;
  }

  for (const auto& operand : instruction.operands) {
    invalidate(operand);
  }
}

}  // namespace

void StackObjectPromotion::run(compiler::backend::mir::MachineModule& module) const {
  for (auto& function : module.functions) {
    std::vector<bool> promotable(function.stackObjects.size(), false);
    for (std::size_t i = 0; i < function.stackObjects.size(); ++i) {
      promotable[i] = isPromotableObject(function.stackObjects[i]);
    }

    for (const auto& block : function.blocks) {
      for (const auto& instruction : block.instructions) {
        markInvalidIfUsedOutsideDirectAccess(instruction, promotable);

        if (instruction.opcode == "load" && !instruction.operands.empty()) {
          const auto& address = instruction.operands[0];
          if (address.kind == MachineOperandKind::FrameIndex && address.index >= 0 &&
              static_cast<std::size_t>(address.index) < promotable.size()) {
            continue;
          }
        }

        if (instruction.opcode == "store" && instruction.operands.size() >= 2) {
          const auto& address = instruction.operands[1];
          if (address.kind == MachineOperandKind::FrameIndex && address.index >= 0 &&
              static_cast<std::size_t>(address.index) < promotable.size()) {
            continue;
          }
        }
      }
    }

    std::unordered_map<int, MachineOperand> promotedRegisters;
    for (std::size_t i = 0; i < promotable.size(); ++i) {
      if (!promotable[i]) {
        continue;
      }
      const ObjectType& objectType = function.stackObjects[i].objectType;
      const std::string regName = "%stack.promoted." + std::to_string(i);
      promotedRegisters.emplace(static_cast<int>(i),
                                MachineOperand::virtualRegister(
                                    regName, ValueType::fromScalarObject(objectType)));
    }

    if (promotedRegisters.empty()) {
      continue;
    }

    for (auto& block : function.blocks) {
      for (auto& instruction : block.instructions) {
        if (instruction.opcode == "load" && instruction.operands.size() == 1) {
          const auto& address = instruction.operands[0];
          if (address.kind == MachineOperandKind::FrameIndex) {
            const auto promoted = promotedRegisters.find(address.index);
            if (promoted != promotedRegisters.end()) {
              instruction = MachineInstruction::copy(instruction.result, promoted->second);
            }
          }
          continue;
        }

        if (instruction.opcode == "store" && instruction.operands.size() == 2) {
          const auto& address = instruction.operands[1];
          if (address.kind == MachineOperandKind::FrameIndex) {
            const auto promoted = promotedRegisters.find(address.index);
            if (promoted != promotedRegisters.end()) {
              instruction = MachineInstruction::copy(promoted->second, instruction.operands[0]);
            }
          }
        }
      }
    }
  }
}

}  // namespace compiler::backend::opt
