#include "Riscv64Target.h"

#include <utility>

#include "TargetLoweringSupport.h"

namespace compiler::backend::target::riscv64 {

namespace {

using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

bool isRegisterLike(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister ||
         operand.kind == MachineOperandKind::PhysicalRegister;
}

void replaceOperand(MachineOperand& operand, const std::string& from, MachineOperand to) {
  if (isRegisterLike(operand) && operand.text == from) {
    operand = std::move(to);
  }
}

}  // namespace

bool InstrAdapter::isCall(const MachineInstruction& instruction) const {
  return instruction.opcode == "call";
}

bool InstrAdapter::isReturn(const MachineInstruction& instruction) const {
  return instruction.opcode == "ret";
}

bool InstrAdapter::isUnconditionalBranch(const MachineInstruction& instruction) const {
  return instruction.opcode == "br" && instruction.operands.size() == 1;
}

bool InstrAdapter::isConditionalBranch(const MachineInstruction& instruction) const {
  return instruction.opcode == "br" && instruction.operands.size() == 3;
}

std::vector<std::string> InstrAdapter::branchTargets(const MachineInstruction& instruction) const {
  std::vector<std::string> targets;
  if (isUnconditionalBranch(instruction)) {
    targets.push_back(instruction.operands[0].text);
  } else if (isConditionalBranch(instruction)) {
    targets.push_back(instruction.operands[1].text);
    targets.push_back(instruction.operands[2].text);
  }
  return targets;
}

void InstrAdapter::enumerateUses(const MachineInstruction& instruction,
                                 std::vector<MachineOperand>& out) const {
  out.clear();

  if (instruction.kind == MachineInstructionKind::Phi) {
    for (const auto& incoming : instruction.phiInputs) {
      if (isRegisterOperand(incoming.value)) {
        out.push_back(incoming.value);
      }
    }
    return;
  }

  const std::size_t begin = (instruction.opcode == "call" && !instruction.operands.empty()) ? 1 : 0;
  for (std::size_t i = begin; i < instruction.operands.size(); ++i) {
    if (isRegisterOperand(instruction.operands[i])) {
      out.push_back(instruction.operands[i]);
    }
  }
}

void InstrAdapter::enumerateDefs(const MachineInstruction& instruction,
                                 std::vector<MachineOperand>& out) const {
  out.clear();
  if (isResultDef(instruction) && isRegisterOperand(instruction.result)) {
    out.push_back(instruction.result);
  }
}

void InstrAdapter::enumeratePhysicalRegisters(const MachineInstruction& instruction,
                                              std::vector<MachineOperand>& out) const {
  out.clear();
  if (instruction.hasResult && instruction.result.kind == MachineOperandKind::PhysicalRegister) {
    out.push_back(instruction.result);
  }

  if (instruction.kind == MachineInstructionKind::Phi) {
    for (const auto& incoming : instruction.phiInputs) {
      if (incoming.value.kind == MachineOperandKind::PhysicalRegister) {
        out.push_back(incoming.value);
      }
    }
    return;
  }

  for (const auto& operand : instruction.operands) {
    if (operand.kind == MachineOperandKind::PhysicalRegister) {
      out.push_back(operand);
    }
  }
}

void InstrAdapter::replaceUse(MachineInstruction& instruction, const std::string& from,
                              MachineOperand to) const {
  if (instruction.kind == MachineInstructionKind::Phi) {
    for (auto& incoming : instruction.phiInputs) {
      replaceOperand(incoming.value, from, to);
    }
    return;
  }

  const std::size_t begin = (instruction.opcode == "call" && !instruction.operands.empty()) ? 1 : 0;
  for (std::size_t i = begin; i < instruction.operands.size(); ++i) {
    replaceOperand(instruction.operands[i], from, to);
  }
}

void InstrAdapter::replaceDef(MachineInstruction& instruction, const std::string& from,
                              MachineOperand to) const {
  if (instruction.hasResult) {
    replaceOperand(instruction.result, from, std::move(to));
  }
}

bool InstrAdapter::isRegisterOperand(const MachineOperand& operand) {
  return isRegisterLike(operand);
}

bool InstrAdapter::isResultDef(const MachineInstruction& instruction) {
  return instruction.hasResult && instruction.result.kind != MachineOperandKind::None;
}

compiler::backend::mir::MachineModule Lowering::lowerModule(
    const compiler::ir::core::Module& module,
    const compiler::backend::target::TargetInfo& target) const {
  return compiler::backend::target::detail::lowerModuleDefault(module, target);
}

}  // namespace compiler::backend::target::riscv64
