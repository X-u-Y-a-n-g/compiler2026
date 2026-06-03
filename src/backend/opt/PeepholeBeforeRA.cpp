#include "../../../include/compiler/backend/opt/PeepholeBeforeRA.h"

#include <vector>

#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;

bool isInt(const MachineOperand& operand, std::int64_t value) {
  return operand.kind == compiler::backend::mir::MachineOperandKind::ImmediateInt &&
         operand.intValue == value;
}

bool simplifyTarget(MachineInstruction& instruction) {
  if (instruction.kind != MachineInstructionKind::Target || !instruction.hasResult ||
      instruction.operands.size() < 2) {
    return false;
  }

  const auto& opcode = instruction.opcode;
  const auto lhs = instruction.operands[0];
  const auto rhs = instruction.operands[1];

  if ((opcode == "add" || opcode == "or") && isInt(rhs, 0)) {
    instruction = MachineInstruction::copy(instruction.result, lhs);
    return true;
  }
  if ((opcode == "add" || opcode == "or") && isInt(lhs, 0)) {
    instruction = MachineInstruction::copy(instruction.result, rhs);
    return true;
  }
  if (opcode == "sub" && isInt(rhs, 0)) {
    instruction = MachineInstruction::copy(instruction.result, lhs);
    return true;
  }
  if (opcode == "mul" && isInt(rhs, 1)) {
    instruction = MachineInstruction::copy(instruction.result, lhs);
    return true;
  }
  if (opcode == "mul" && isInt(lhs, 1)) {
    instruction = MachineInstruction::copy(instruction.result, rhs);
    return true;
  }
  if (opcode == "mul" && (isInt(lhs, 0) || isInt(rhs, 0))) {
    instruction = MachineInstruction::copy(
        instruction.result, MachineOperand::immediateInt(0, instruction.result.type));
    return true;
  }
  if (opcode == "and" && isInt(rhs, 0)) {
    instruction = MachineInstruction::copy(
        instruction.result, MachineOperand::immediateInt(0, instruction.result.type));
    return true;
  }
  if (opcode == "and" && isInt(lhs, 0)) {
    instruction = MachineInstruction::copy(
        instruction.result, MachineOperand::immediateInt(0, instruction.result.type));
    return true;
  }
  if (opcode == "and" && isInt(rhs, -1)) {
    instruction = MachineInstruction::copy(instruction.result, lhs);
    return true;
  }
  if (opcode == "and" && isInt(lhs, -1)) {
    instruction = MachineInstruction::copy(instruction.result, rhs);
    return true;
  }
  return false;
}

}  // namespace

void PeepholeBeforeRA::run(compiler::backend::mir::MachineModule& module) const {
  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      std::vector<MachineInstruction> rewritten;
      rewritten.reserve(block.instructions.size());

      for (auto instruction : block.instructions) {
        simplifyTarget(instruction);
        if (instruction.kind == MachineInstructionKind::Copy && instruction.hasResult &&
            !instruction.operands.empty() &&
            detail::sameOperand(instruction.result, instruction.operands[0])) {
          continue;
        }
        rewritten.push_back(std::move(instruction));
      }

      block.instructions = std::move(rewritten);
    }
  }
}

}  // namespace compiler::backend::opt
