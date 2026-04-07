#pragma once

#include <string>
#include <vector>

#include "compiler/support/SourceLocation.h"

namespace compiler::frontend::lexer {

enum class TokenKind {
  EndOfFile,
  Identifier,
  Number,
  LongLongLiteral,
  FloatLiteral,
  StringLiteral,
  KeywordInt,
  KeywordFloat,
  KeywordVoid,
  KeywordBool,
  KeywordLong,
  KeywordConst,
  KeywordReturn,
  KeywordIf,
  KeywordElse,
  KeywordFor,
  KeywordWhile,
  KeywordContinue,
  KeywordBreak,
  KeywordSwitch,
  KeywordCase,
  KeywordDefault,
  KeywordGoto,
  KeywordDo,
  LParen,
  RParen,
  LBracket,
  RBracket,
  LBrace,
  RBrace,
  Comma,
  Colon,
  Semicolon,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  EqualEqual,
  NotEqual,
  LogicalAnd,
  LogicalOr,
  LogicalNot,
  Ampersand,
  Pipe,
  Assign,
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
