#include "../../../include/compiler/backend/opt/MachineConstantFold.h"

#include <cstdint>

#include "MachineOptUtils.h"

namespace compiler::backend::opt {

void MachineConstantFold::run(compiler::backend::mir::MachineModule& module) const {
  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      for (auto& instruction : block.instructions) {
        if (instruction.kind != compiler::backend::mir::MachineInstructionKind::Target ||
            !instruction.hasResult || instruction.operands.size() < 2 ||
            !detail::isImmediateInt(instruction.operands[0]) ||
            !detail::isImmediateInt(instruction.operands[1])) {
          continue;
        }

        std::int64_t folded = 0;
        const auto lhs = instruction.operands[0].intValue;
        const auto rhs = instruction.operands[1].intValue;
        if (!detail::checkedBinary(instruction.opcode, lhs, rhs, folded) &&
            !detail::checkedCompare(instruction.opcode, lhs, rhs, folded)) {
          continue;
        }

        instruction = compiler::backend::mir::MachineInstruction::copy(
            instruction.result,
            compiler::backend::mir::MachineOperand::immediateInt(folded, instruction.result.type));
      }
    }
  }
}

}  // namespace compiler::backend::opt
