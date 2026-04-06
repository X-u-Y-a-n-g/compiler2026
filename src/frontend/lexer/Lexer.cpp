#include "compiler/frontend/lexer/Lexer.h"

#include <cctype>
#include <sstream>
#include <unordered_map>

namespace compiler::frontend::lexer {

namespace {

const std::unordered_map<std::string, TokenKind> kKeywords = {
    {"int", TokenKind::KeywordInt},
    {"return", TokenKind::KeywordReturn},
    {"if", TokenKind::KeywordIf},
    {"else", TokenKind::KeywordElse},
    {"while", TokenKind::KeywordWhile},
};

}  // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

std::vector<Token> Lexer::lex(compiler::support::DiagnosticEngine& diagnostics) {
  std::vector<Token> tokens;

  while (!isAtEnd()) {
    skipWhitespace();
    if (isAtEnd()) {
      break;
    }

    const auto start = location();
    const char current = peek();
    if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
      tokens.push_back(lexIdentifier());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(current))) {
      tokens.push_back(lexNumber());
      continue;
    }

    advance();
    Token token;
    token.location = start;
    token.lexeme = std::string(1, current);
    switch (current) {
      case '(':
        token.kind = TokenKind::LParen;
        break;
      case ')':
        token.kind = TokenKind::RParen;
        break;
      case '{':
        token.kind = TokenKind::LBrace;
        break;
      case '}':
        token.kind = TokenKind::RBrace;
        break;
      case ',':
        token.kind = TokenKind::Comma;
        break;
      case ';':
        token.kind = TokenKind::Semicolon;
        break;
      case '+':
        token.kind = TokenKind::Plus;
        break;
      case '-':
        token.kind = TokenKind::Minus;
        break;
      case '*':
        token.kind = TokenKind::Star;
        break;
      case '/':
        token.kind = TokenKind::Slash;
        break;
      case '%':
        token.kind = TokenKind::Percent;
        break;
      case '=':
        token.kind = TokenKind::Equal;
        break;
      default:
        token.kind = TokenKind::Unknown;
        diagnostics.warning(start, std::string("unknown character: ") + current);
        break;
    }
    tokens.push_back(token);
  }

  tokens.push_back(Token{TokenKind::EndOfFile, "", location()});
  return tokens;
}

char Lexer::peek() const {
  return source_[index_];
}

char Lexer::advance() {
  const char current = source_[index_++];
  if (current == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return current;
}

bool Lexer::isAtEnd() const {
  return index_ >= source_.size();
}

void Lexer::skipWhitespace() {
  while (!isAtEnd()) {
    const char current = peek();
    if (current == ' ' || current == '\t' || current == '\r' || current == '\n') {
      advance();
      continue;
    }
    break;
  }
}

Token Lexer::lexIdentifier() {
  const auto start = location();
  std::string lexeme;
  while (!isAtEnd()) {
    const char current = peek();
    if (!std::isalnum(static_cast<unsigned char>(current)) && current != '_') {
      break;
    }
    lexeme.push_back(advance());
  }

  TokenKind kind = TokenKind::Identifier;
  if (const auto it = kKeywords.find(lexeme); it != kKeywords.end()) {
    kind = it->second;
  }
  return Token{kind, lexeme, start};
}

Token Lexer::lexNumber() {
  const auto start = location();
  std::string lexeme;
  while (!isAtEnd()) {
    const char current = peek();
    if (!std::isdigit(static_cast<unsigned char>(current))) {
      break;
    }
    lexeme.push_back(advance());
  }
  return Token{TokenKind::Number, lexeme, start};
}

compiler::support::SourceLocation Lexer::location() const {
  return compiler::support::SourceLocation{index_, line_, column_};
}

std::string toString(TokenKind kind) {
  switch (kind) {
    case TokenKind::EndOfFile:
      return "eof";
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::Number:
      return "number";
    case TokenKind::KeywordInt:
      return "kw_int";
    case TokenKind::KeywordReturn:
      return "kw_return";
    case TokenKind::KeywordIf:
      return "kw_if";
    case TokenKind::KeywordElse:
      return "kw_else";
    case TokenKind::KeywordWhile:
      return "kw_while";
    case TokenKind::LParen:
      return "l_paren";
    case TokenKind::RParen:
      return "r_paren";
    case TokenKind::LBrace:
      return "l_brace";
    case TokenKind::RBrace:
      return "r_brace";
    case TokenKind::Comma:
      return "comma";
    case TokenKind::Semicolon:
      return "semicolon";
    case TokenKind::Plus:
      return "plus";
    case TokenKind::Minus:
      return "minus";
    case TokenKind::Star:
      return "star";
    case TokenKind::Slash:
      return "slash";
    case TokenKind::Percent:
      return "percent";
    case TokenKind::Equal:
      return "equal";
    case TokenKind::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string dumpTokens(const std::vector<Token>& tokens) {
  std::ostringstream os;
  for (const auto& token : tokens) {
    os << token.location.toString() << "  " << toString(token.kind);
    if (!token.lexeme.empty()) {
      os << "  `" << token.lexeme << '`';
    }
    os << '\n';
  }
  return os.str();
}

}  // namespace compiler::frontend::lexer
