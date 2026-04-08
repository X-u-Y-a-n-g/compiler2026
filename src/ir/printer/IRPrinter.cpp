#include "compiler/ir/printer/IRPrinter.h"

#include <sstream>

namespace compiler::ir::printer {

namespace {

std::string printConstant(const compiler::ir::core::Constant& constant) {
  if (constant.isScalar()) {
    return constant.literal();
  }

  std::ostringstream os;
  os << "[";
  for (std::size_t i = 0; i < constant.elements().size(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << printConstant(constant.elements()[i]);
  }
  os << "]";
  return os.str();
}

void printFunctionSignature(std::ostringstream& os, const compiler::ir::core::Function& function) {
  os << "func @" << function.name() << "(";
  for (std::size_t i = 0; i < function.parameters().size(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << function.parameters()[i].name << ": " << function.parameters()[i].type;
  }
  os << ") -> " << function.returnType() << "\n";
}

}  // namespace

std::string IRPrinter::print(const compiler::ir::core::Module& module) const {
  std::ostringstream os;
  os << "module \"" << module.sourceName() << "\"\n";
  for (const auto& global : module.globals()) {
    os << "global @" << global.name() << " : ";
    if (global.isConst()) {
      os << "const ";
    }
    os << global.type() << " = " << printConstant(global.initializer()) << '\n';
  }
  for (const auto& note : module.passLog()) {
    os << "; pass: " << note << '\n';
  }
  for (const auto& function : module.functions()) {
    printFunctionSignature(os, function);
    for (const auto& block : function.blocks()) {
      os << block.name() << ":\n";
      for (const auto& instruction : block.instructions()) {
        os << "  ";
        if (instruction.hasResult()) {
          os << instruction.result() << " = ";
        }
        os << instruction.opcode();
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
