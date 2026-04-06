#include "compiler/backend/asm/AsmPrinter.h"

#include <sstream>

namespace compiler::backend::asmcode {

std::string AsmPrinter::print(const compiler::backend::mir::MachineModule& module) const {
  std::ostringstream os;
  os << ".text\n";

  for (const auto& function : module.functions) {
    os << ".globl " << function.name << '\n';
    os << function.name << ":\n";

    for (const auto& block : function.blocks) {
      if (block.name != "entry") {
        os << block.name << ":\n";
      }

      for (const auto& instruction : block.instructions) {
        if (module.target.name() == "armv7") {
          if (instruction.opcode == "mov_ret") {
            os << "  mov r0, #" << instruction.operands.front() << '\n';
          } else if (instruction.opcode == "ret") {
            os << "  bx lr\n";
          }
        } else {
          if (instruction.opcode == "load_ret") {
            os << "  li a0, " << instruction.operands.front() << '\n';
          } else if (instruction.opcode == "ret") {
            os << "  ret\n";
          }
        }
      }
    }
  }

  return os.str();
}

}  // namespace compiler::backend::asmcode
