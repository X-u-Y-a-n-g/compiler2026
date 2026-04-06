#pragma once

#include "compiler/frontend/ast/AstNode.h"
#include "compiler/support/Diagnostic.h"

namespace compiler::frontend::sema {

class SemanticAnalyzer {
 public:
  bool analyze(const compiler::frontend::ast::TranslationUnit& unit,
               compiler::support::DiagnosticEngine& diagnostics) const;
};

}  // namespace compiler::frontend::sema
