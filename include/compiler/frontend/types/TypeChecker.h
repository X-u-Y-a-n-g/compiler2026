#pragma once

#include "compiler/frontend/ast/AstNode.h"
#include "compiler/frontend/types/Type.h"
#include "compiler/support/Diagnostic.h"

namespace compiler::frontend::types {

class TypeChecker {
 public:
  bool check(const compiler::frontend::ast::TranslationUnit& unit,
             compiler::support::DiagnosticEngine& diagnostics) const;
};

}  // namespace compiler::frontend::types
