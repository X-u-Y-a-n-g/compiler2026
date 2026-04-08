#include "compiler/backend/frame/FrameLowering.h"

#include <algorithm>
#include <cstddef>

namespace compiler::backend::frame {

namespace {

std::size_t alignTo(std::size_t value, std::size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : (value + alignment - remainder);
}

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
          if (floatRegs < 8) {
            ++floatRegs;
          } else {
            stackBytes += std::max<std::size_t>(8, target.pointerSize());
          }
        } else {
          if (integerRegs < 8) {
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
  const std::size_t saveArea = module.target.pointerSize() * 2;

  for (auto& function : module.functions) {
    function.savedRaOffset = static_cast<int>(module.target.pointerSize());
    function.savedFpOffset = static_cast<int>(module.target.pointerSize() * 2);
    function.outgoingArgSize = requiredOutgoingArgSize(function, module.target);

    std::size_t cursor = saveArea;
    for (auto& object : function.stackObjects) {
      cursor = alignTo(cursor, object.alignment);
      cursor += object.size;
      object.offset = static_cast<int>(cursor);
    }

    cursor += function.outgoingArgSize;
    function.frameSize = alignTo(cursor, module.target.stackAlignment());
    if (function.frameSize == 0) {
      function.frameSize = module.target.stackAlignment();
    }
  }
}

}  // namespace compiler::backend::frame
