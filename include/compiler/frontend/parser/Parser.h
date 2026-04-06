#pragma once

#include <vector>

#include "compiler/frontend/ast/AstNode.h"
#include "compiler/frontend/lexer/Token.h"
#include "compiler/support/Diagnostic.h"

namespace compiler::frontend::parser {

class Parser {
 public:
  explicit Parser(std::vector<compiler::frontend::lexer::Token> tokens);

  compiler::frontend::ast::TranslationUnitPtr parse(
      compiler::support::DiagnosticEngine& diagnostics);

 private:
  std::vector<compiler::frontend::lexer::Token> tokens_;
};

}  // namespace compiler::frontend::parser
