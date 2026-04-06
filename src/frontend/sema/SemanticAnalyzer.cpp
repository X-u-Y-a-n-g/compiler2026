#include "compiler/frontend/sema/SemanticAnalyzer.h"

namespace compiler::frontend::sema {

bool SemanticAnalyzer::analyze(const compiler::frontend::ast::TranslationUnit&,
                               compiler::support::DiagnosticEngine&) const {
  return true;
}

}  // namespace compiler::frontend::sema
