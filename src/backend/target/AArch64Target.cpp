#include "../../../include/compiler/backend/target/AArch64Target.h"

#include <utility>

#include "../../../include/compiler/backend/target/TargetLoweringSupport.h"

namespace compiler::backend::target::aarch64 {
namespace {
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

bool isRegister(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister || operand.kind == MachineOperandKind::PhysicalRegister;
}
void replace(MachineOperand& operand, const std::string& from, const MachineOperand& to) {
  if (isRegister(operand) && operand.text == from) operand = to;
}
}  // namespace

bool InstrAdapter::isCall(const MachineInstruction& i) const { return i.opcode == "call"; }
bool InstrAdapter::isReturn(const MachineInstruction& i) const { return i.opcode == "ret"; }
bool InstrAdapter::isUnconditionalBranch(const MachineInstruction& i) const { return i.opcode == "br" && i.operands.size() == 1; }
bool InstrAdapter::isConditionalBranch(const MachineInstruction& i) const {
  return (i.opcode == "br" && i.operands.size() == 3) ||
         (i.opcode.rfind("br.icmp.", 0) == 0 && i.operands.size() == 4);
}
std::vector<std::string> InstrAdapter::branchTargets(const MachineInstruction& i) const {
  if (isUnconditionalBranch(i)) return {i.operands[0].text};
  if (isConditionalBranch(i)) {
    const std::size_t n = i.opcode == "br" ? 1 : 2;
    return {i.operands[n].text, i.operands[n + 1].text};
  }
  return {};
}
void InstrAdapter::enumerateUses(const MachineInstruction& i, std::vector<MachineOperand>& out) const {
  out.clear();
  if (i.kind == MachineInstructionKind::Phi) {
    for (const auto& incoming : i.phiInputs) if (isRegister(incoming.value)) out.push_back(incoming.value);
    return;
  }
  const std::size_t begin = i.opcode == "call" && !i.operands.empty() ? 1 : 0;
  for (std::size_t n = begin; n < i.operands.size(); ++n) if (isRegister(i.operands[n])) out.push_back(i.operands[n]);
}
void InstrAdapter::enumerateDefs(const MachineInstruction& i, std::vector<MachineOperand>& out) const {
  out.clear();
  if (i.hasResult && isRegister(i.result)) out.push_back(i.result);
}
void InstrAdapter::enumeratePhysicalRegisters(const MachineInstruction& i, std::vector<MachineOperand>& out) const {
  out.clear();
  if (i.hasResult && i.result.kind == MachineOperandKind::PhysicalRegister) out.push_back(i.result);
  if (i.kind == MachineInstructionKind::Phi) {
    for (const auto& incoming : i.phiInputs) if (incoming.value.kind == MachineOperandKind::PhysicalRegister) out.push_back(incoming.value);
  } else {
    for (const auto& operand : i.operands) if (operand.kind == MachineOperandKind::PhysicalRegister) out.push_back(operand);
  }
}
void InstrAdapter::replaceUse(MachineInstruction& i, const std::string& from, MachineOperand to) const {
  if (i.kind == MachineInstructionKind::Phi) {
    for (auto& incoming : i.phiInputs) replace(incoming.value, from, to);
    return;
  }
  const std::size_t begin = i.opcode == "call" && !i.operands.empty() ? 1 : 0;
  for (std::size_t n = begin; n < i.operands.size(); ++n) replace(i.operands[n], from, to);
}
void InstrAdapter::replaceDef(MachineInstruction& i, const std::string& from, MachineOperand to) const {
  if (i.hasResult) replace(i.result, from, to);
}
compiler::backend::mir::MachineModule Lowering::lowerModule(
    const compiler::ir::core::Module& module, const compiler::backend::target::TargetInfo& target) const {
  return compiler::backend::target::detail::lowerModuleDefault(module, target);
}
}  // namespace compiler::backend::target::aarch64
