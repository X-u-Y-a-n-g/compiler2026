#pragma once

#include <string>
#include <vector>

#include "compiler/frontend/lexer/Token.h"
#include "compiler/support/Diagnostic.h"

namespace compiler::frontend::lexer {

class Lexer {
 public:
  explicit Lexer(std::string source);

  std::vector<Token> lex(compiler::support::DiagnosticEngine& diagnostics);

 private:
  char peek() const;
  char advance();
  bool isAtEnd() const;
  void skipWhitespace();
  Token lexIdentifier();
  Token lexNumber();
  compiler::support::SourceLocation location() const;

  std::string source_;
  std::size_t index_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

}  // namespace compiler::frontend::lexer
