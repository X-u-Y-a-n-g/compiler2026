#include "../../../include/compiler/backend/opt/PeepholeAfterRA.h"

#include <vector>

#include "MachineOptUtils.h"

namespace compiler::backend::opt {

void PeepholeAfterRA::run(compiler::backend::mir::MachineModule& module) const {
  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      std::vector<compiler::backend::mir::MachineInstruction> rewritten;
      rewritten.reserve(block.instructions.size());

      for (auto instruction : block.instructions) {
        if (instruction.kind == compiler::backend::mir::MachineInstructionKind::Copy &&
            instruction.hasResult && !instruction.operands.empty() &&
            detail::sameOperand(instruction.result, instruction.operands[0])) {
          continue;
        }
        if (instruction.opcode == "stack.adjust" && !instruction.operands.empty() &&
            instruction.operands[0].kind ==
                compiler::backend::mir::MachineOperandKind::ImmediateInt &&
            instruction.operands[0].intValue == 0) {
          continue;
        }
        rewritten.push_back(std::move(instruction));
      }

      block.instructions = std::move(rewritten);
    }
  }
}

}  // namespace compiler::backend::opt
