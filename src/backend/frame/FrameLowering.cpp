#include "../../../include/compiler/backend/frame/FrameLowering.h"

#include <algorithm>
#include <cstddef>

namespace compiler::backend::frame {

namespace {

std::size_t requiredOutgoingArgSize(const compiler::backend::mir::MachineFunction& function,
                                    const compiler::backend::target::TargetInfo& target) {
  std::size_t maxBytes = 0;

  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instruction.opcode != "call") {
        continue;
      }

      std::size_t integerRegs = 0;
      std::size_t floatRegs = 0;
      std::size_t stackBytes = 0;
      for (std::size_t i = 1; i < instruction.operands.size(); ++i) {
        const auto& operand = instruction.operands[i];
        if (operand.type.isFloat()) {
          if (floatRegs < target.floatArgRegisters().size()) {
            ++floatRegs;
          } else {
            stackBytes += std::max<std::size_t>(8, target.pointerSize());
          }
        } else {
          if (integerRegs < target.integerArgRegisters().size()) {
            ++integerRegs;
          } else {
            stackBytes += std::max<std::size_t>(8, target.pointerSize());
          }
        }
      }
      maxBytes = std::max(maxBytes, stackBytes);
    }
  }

  return maxBytes;
}

}  // namespace

void FrameLowering::lower(compiler::backend::mir::MachineModule& module) const {
  for (auto& function : module.functions) {
    function.outgoingArgSize = requiredOutgoingArgSize(function, module.target);
  }
}

}  // namespace compiler::backend::frame
