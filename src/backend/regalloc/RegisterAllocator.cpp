#include "compiler/backend/regalloc/RegisterAllocator.h"

#include <unordered_map>

namespace compiler::backend::regalloc {

namespace {

int ensureSlot(compiler::backend::mir::MachineFunction& function,
               std::unordered_map<std::string, int>& slotMap, const std::string& name,
               const compiler::backend::mir::ValueType& type,
               const compiler::backend::target::TargetInfo& target) {
  const auto found = slotMap.find(name);
  if (found != slotMap.end()) {
    return found->second;
  }

  compiler::backend::mir::StackObject object;
  object.kind = compiler::backend::mir::StackObjectKind::Spill;
  object.name = name;
  object.size = type.size(target);
  object.alignment = type.alignment(target);
  function.stackObjects.push_back(object);

  const int slot = static_cast<int>(function.stackObjects.size() - 1);
  slotMap.emplace(name, slot);
  return slot;
}

void rewriteOperand(compiler::backend::mir::MachineOperand& operand,
                    compiler::backend::mir::MachineFunction& function,
                    std::unordered_map<std::string, int>& slotMap,
                    const compiler::backend::target::TargetInfo& target) {
  if (operand.kind != compiler::backend::mir::MachineOperandKind::VirtualRegister) {
    return;
  }
  operand = compiler::backend::mir::MachineOperand::stackSlot(
      ensureSlot(function, slotMap, operand.text, operand.type, target), operand.type);
}

}  // namespace

void RegisterAllocator::allocate(compiler::backend::mir::MachineModule& module) const {
  for (auto& function : module.functions) {
    std::unordered_map<std::string, int> slotMap;

    for (auto& parameter : function.parameters) {
      parameter.homeSlot = ensureSlot(function, slotMap, parameter.name, parameter.type, module.target);
    }

    for (auto& block : function.blocks) {
      for (auto& instruction : block.instructions) {
        if (instruction.hasResult) {
          rewriteOperand(instruction.result, function, slotMap, module.target);
        }
        for (auto& operand : instruction.operands) {
          rewriteOperand(operand, function, slotMap, module.target);
        }
      }
    }
  }
}

}  // namespace compiler::backend::regalloc
