#pragma once

#include <string>
#include <vector>

#include "compiler/support/SourceLocation.h"

namespace compiler::frontend::lexer {

enum class TokenKind {
  EndOfFile,
  Identifier,
  Number,
  KeywordInt,
  KeywordReturn,
  KeywordIf,
  KeywordElse,
  KeywordWhile,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Comma,
  Semicolon,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Equal,
  Unknown,
};

struct Token {
  TokenKind kind = TokenKind::Unknown;
  std::string lexeme;
  compiler::support::SourceLocation location;
};

std::string toString(TokenKind kind);
std::string dumpTokens(const std::vector<Token>& tokens);

}  // namespace compiler::frontend::lexer
