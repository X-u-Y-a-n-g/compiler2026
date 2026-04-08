#include "compiler/ir/printer/IRPrinter.h"

#include <sstream>

namespace compiler::ir::printer {

std::string IRPrinter::print(const compiler::ir::core::Module& module) const {
  std::ostringstream os;
  os << "module \"" << module.sourceName() << "\"\n";
  for (const auto& global : module.globals()) {
    os << "global " << global << '\n';
  }
  for (const auto& note : module.passLog()) {
    os << "; pass: " << note << '\n';
  }
  for (const auto& function : module.functions()) {
    os << "func @" << function.name() << "\n";
    for (const auto& block : function.blocks()) {
      os << block.name() << ":\n";
      for (const auto& instruction : block.instructions()) {
        os << "  " << instruction.opcode();
        for (const auto& operand : instruction.operands()) {
          os << ' ' << operand;
        }
        os << '\n';
      }
    }
  }
  return os.str();
}

}  // namespace compiler::ir::printer
