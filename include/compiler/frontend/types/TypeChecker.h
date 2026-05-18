#pragma once

#include "AstNode.h"
#include "Type.h"
#include "Diagnostic.h"

namespace compiler::frontend::types {

class TypeChecker {
 public:
  bool check(const compiler::frontend::ast::TranslationUnit& unit,
             compiler::support::DiagnosticEngine& diagnostics) const;
};

}  // namespace compiler::frontend::types
