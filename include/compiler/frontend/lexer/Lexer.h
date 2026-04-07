#pragma once

#include <string_view>
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
  char peek(std::size_t offset = 0) const;
  char advance();
  bool isAtEnd() const;
  bool startsWith(std::string_view text) const;
  void skipIgnored(compiler::support::DiagnosticEngine& diagnostics);
  void skipLineComment();
  void skipBlockComment(compiler::support::DiagnosticEngine& diagnostics);
  Token lexIdentifier();
  Token lexNumber(compiler::support::DiagnosticEngine& diagnostics);
  Token lexStringLiteral(compiler::support::DiagnosticEngine& diagnostics);
  Token lexPunctuationOrOperator(compiler::support::DiagnosticEngine& diagnostics);
  compiler::support::SourceLocation location() const;

  std::string source_;
  std::size_t index_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

}  // namespace compiler::frontend::lexer
