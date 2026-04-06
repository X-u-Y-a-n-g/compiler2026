#include "compiler/frontend/ast/AstPrinter.h"

#include <sstream>

namespace compiler::frontend::ast {

std::string AstPrinter::print(const TranslationUnit& unit) {
  output_.clear();
  unit.accept(*this);
  return output_;
}

void AstPrinter::visit(const TranslationUnit& node) {
  std::ostringstream os;
  os << "TranslationUnit\n";
  os << "  token_count: " << node.tokens().size() << '\n';
  output_ = os.str();
}

}  // namespace compiler::frontend::ast
