#include "../../../include/compiler/frontend/lexer/Lexer.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace compiler::frontend::lexer {

namespace {

const std::unordered_map<std::string, TokenKind> kKeywords = {
    {"int", TokenKind::KeywordInt},
    {"float", TokenKind::KeywordFloat},
    {"void", TokenKind::KeywordVoid},
    {"bool", TokenKind::KeywordBool},
    {"long", TokenKind::KeywordLong},
    {"const", TokenKind::KeywordConst},
    {"return", TokenKind::KeywordReturn},
    {"if", TokenKind::KeywordIf},
    {"else", TokenKind::KeywordElse},
    {"for", TokenKind::KeywordFor},
    {"while", TokenKind::KeywordWhile},
    {"continue", TokenKind::KeywordContinue},
    {"break", TokenKind::KeywordBreak},
    {"switch", TokenKind::KeywordSwitch},
    {"case", TokenKind::KeywordCase},
    {"default", TokenKind::KeywordDefault},
    {"goto", TokenKind::KeywordGoto},
    {"do", TokenKind::KeywordDo},
};

const std::regex kHexFloatPattern(
    R"(0[xX]((([0-9a-fA-F]*\.[0-9a-fA-F]+)|([0-9a-fA-F]+\.[0-9a-fA-F]*))([pP][+-]?[0-9]+)[fF]?|([0-9a-fA-F]+[pP][+-]?[0-9]+[fF]?)))");
const std::regex kDecFloatPattern(
    R"((([0-9]*\.[0-9]+([eE][+-]?[0-9]+)?[fF]?)|([0-9]+\.[0-9]*([eE][+-]?[0-9]+)?[fF]?)|([0-9]+[eE][+-]?[0-9]+[fF]?)))");
const std::regex kHexIntPattern(R"(0[xX][0-9a-fA-F]+)");
const std::regex kOctIntPattern(R"(0[0-7]*)");
const std::regex kDecIntPattern(R"([1-9][0-9]*)");

Token makeToken(TokenKind kind, const std::string& lexeme,
                const compiler::support::SourceLocation& location) {
  return Token{kind, lexeme, location};
}

Token classifyIntegerLiteral(const std::string& lexeme,
                             const compiler::support::SourceLocation& location,
                             compiler::support::DiagnosticEngine& diagnostics) {
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(lexeme.c_str(), &end, 0);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    diagnostics.warning(location, "invalid integer literal: " + lexeme);
    return makeToken(TokenKind::Unknown, lexeme, location);
  }

  if (value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return makeToken(TokenKind::LongLongLiteral, lexeme, location);
  }
  return makeToken(TokenKind::Number, lexeme, location);
}

Token classifyFloatLiteral(const std::string& lexeme,
                           const compiler::support::SourceLocation& location,
                           compiler::support::DiagnosticEngine& diagnostics) {
  errno = 0;
  char* end = nullptr;
  std::strtof(lexeme.c_str(), &end);
  if (errno == ERANGE || end == nullptr ||
      (*end != '\0' && !(*end == 'f' || *end == 'F') && *(end + 1) != '\0')) {
    diagnostics.warning(location, "invalid floating-point literal: " + lexeme);
    return makeToken(TokenKind::Unknown, lexeme, location);
  }
  return makeToken(TokenKind::FloatLiteral, lexeme, location);
}

}  // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

std::vector<Token> Lexer::lex(compiler::support::DiagnosticEngine& diagnostics) {
  std::vector<Token> tokens;

  while (!isAtEnd()) {
    skipIgnored(diagnostics);
    if (isAtEnd()) {
      break;
    }

    const char current = peek();
    if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
      tokens.push_back(lexIdentifier());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(current))) {
      tokens.push_back(lexNumber(diagnostics));
      continue;
    }
    if (current == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
      tokens.push_back(lexNumber(diagnostics));
      continue;
    }
    if (current == '"') {
      tokens.push_back(lexStringLiteral(diagnostics));
      continue;
    }

    tokens.push_back(lexPunctuationOrOperator(diagnostics));
  }

  tokens.push_back(Token{TokenKind::EndOfFile, "", location()});
  return tokens;
}

char Lexer::peek(std::size_t offset) const {
  const std::size_t target = index_ + offset;
  if (target >= source_.size()) {
    return '\0';
  }
  return source_[target];
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

bool Lexer::startsWith(std::string_view text) const {
  if (index_ + text.size() > source_.size()) {
    return false;
  }
  return std::string_view(source_.data() + index_, text.size()) == text;
}

void Lexer::skipIgnored(compiler::support::DiagnosticEngine& diagnostics) {
  bool advanced = true;
  while (!isAtEnd() && advanced) {
    advanced = false;

    while (!isAtEnd()) {
      const char current = peek();
      if (current == ' ' || current == '\t' || current == '\r' || current == '\n' ||
          current == '\f' || current == '\v') {
        advance();
        advanced = true;
        continue;
      }
      break;
    }

    if (startsWith("//")) {
      skipLineComment();
      advanced = true;
      continue;
    }

    if (startsWith("/*")) {
      skipBlockComment(diagnostics);
      advanced = true;
    }
  }
}

void Lexer::skipLineComment() {
  advance();
  advance();
  while (!isAtEnd() && peek() != '\n') {
    advance();
  }
}

void Lexer::skipBlockComment(compiler::support::DiagnosticEngine& diagnostics) {
  const auto start = location();
  advance();
  advance();

  while (!isAtEnd()) {
    if (startsWith("*/")) {
      advance();
      advance();
      return;
    }
    advance();
  }

  diagnostics.error(start, "unterminated block comment");
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

Token Lexer::lexNumber(compiler::support::DiagnosticEngine& diagnostics) {
  const auto start = location();
  const std::string remaining = source_.substr(index_);
  std::smatch match;

  auto consumeMatch = [&](const std::smatch& currentMatch) -> std::string {
    std::string lexeme = currentMatch.str();
    for (std::size_t i = 0; i < lexeme.size(); ++i) {
      advance();
    }
    return lexeme;
  };

  if (std::regex_search(remaining, match, kHexFloatPattern,
                        std::regex_constants::match_continuous)) {
    return classifyFloatLiteral(consumeMatch(match), start, diagnostics);
  }
  if (std::regex_search(remaining, match, kDecFloatPattern,
                        std::regex_constants::match_continuous)) {
    return classifyFloatLiteral(consumeMatch(match), start, diagnostics);
  }

  if (peek() == '0' && std::isdigit(static_cast<unsigned char>(peek(1))) &&
      peek(1) != 'x' && peek(1) != 'X') {
    bool invalidOctal = false;
    std::string lexeme;
    lexeme.push_back(advance());
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
      if (peek() < '0' || peek() > '7') {
        invalidOctal = true;
      }
      lexeme.push_back(advance());
    }
    if (invalidOctal) {
      diagnostics.warning(start, "invalid octal literal: " + lexeme);
      return makeToken(TokenKind::Unknown, lexeme, start);
    }
    return classifyIntegerLiteral(lexeme, start, diagnostics);
  }

  if (std::regex_search(remaining, match, kHexIntPattern,
                        std::regex_constants::match_continuous)) {
    return classifyIntegerLiteral(consumeMatch(match), start, diagnostics);
  }
  if (startsWith("0x") || startsWith("0X")) {
    std::string lexeme;
    lexeme.push_back(advance());
    lexeme.push_back(advance());
    while (std::isxdigit(static_cast<unsigned char>(peek()))) {
      lexeme.push_back(advance());
    }
    diagnostics.warning(start, "invalid hexadecimal literal: " + lexeme);
    return makeToken(TokenKind::Unknown, lexeme, start);
  }
  if (std::regex_search(remaining, match, kOctIntPattern,
                        std::regex_constants::match_continuous)) {
    return classifyIntegerLiteral(consumeMatch(match), start, diagnostics);
  }
  if (std::regex_search(remaining, match, kDecIntPattern,
                        std::regex_constants::match_continuous)) {
    return classifyIntegerLiteral(consumeMatch(match), start, diagnostics);
  }

  const char current = advance();
  diagnostics.warning(start, std::string("invalid numeric literal starting with: ") + current);
  return makeToken(TokenKind::Unknown, std::string(1, current), start);
}

Token Lexer::lexStringLiteral(compiler::support::DiagnosticEngine& diagnostics) {
  const auto start = location();
  std::string lexeme;

  advance();
  while (!isAtEnd()) {
    const char current = advance();
    if (current == '"') {
      return makeToken(TokenKind::StringLiteral, lexeme, start);
    }
    if (current == '\\') {
      if (isAtEnd()) {
        break;
      }
      const char escaped = advance();
      switch (escaped) {
        case 'n':
          lexeme.push_back('\n');
          break;
        case 't':
          lexeme.push_back('\t');
          break;
        case 'r':
          lexeme.push_back('\r');
          break;
        case '\\':
          lexeme.push_back('\\');
          break;
        case '"':
          lexeme.push_back('"');
          break;
        default:
          lexeme.push_back(escaped);
          break;
      }
      continue;
    }
    if (current == '\n') {
      diagnostics.error(start, "unterminated string literal");
      return makeToken(TokenKind::Unknown, lexeme, start);
    }
    lexeme.push_back(current);
  }

  diagnostics.error(start, "unterminated string literal");
  return makeToken(TokenKind::Unknown, lexeme, start);
}

Token Lexer::lexPunctuationOrOperator(compiler::support::DiagnosticEngine& diagnostics) {
  const auto start = location();
  const char current = advance();
  std::string lexeme(1, current);

  auto makeSingle = [&](TokenKind kind) {
    return makeToken(kind, lexeme, start);
  };

  switch (current) {
    case '(':
      return makeSingle(TokenKind::LParen);
    case ')':
      return makeSingle(TokenKind::RParen);
    case '[':
      return makeSingle(TokenKind::LBracket);
    case ']':
      return makeSingle(TokenKind::RBracket);
    case '{':
      return makeSingle(TokenKind::LBrace);
    case '}':
      return makeSingle(TokenKind::RBrace);
    case ',':
      return makeSingle(TokenKind::Comma);
    case ':':
      return makeSingle(TokenKind::Colon);
    case ';':
      return makeSingle(TokenKind::Semicolon);
    case '+':
      return makeSingle(TokenKind::Plus);
    case '-':
      return makeSingle(TokenKind::Minus);
    case '*':
      return makeSingle(TokenKind::Star);
    case '/':
      return makeSingle(TokenKind::Slash);
    case '%':
      return makeSingle(TokenKind::Percent);
    case '&':
      if (peek() == '&') {
        lexeme.push_back(advance());
        return makeToken(TokenKind::LogicalAnd, lexeme, start);
      }
      return makeSingle(TokenKind::Ampersand);
    case '|':
      if (peek() == '|') {
        lexeme.push_back(advance());
        return makeToken(TokenKind::LogicalOr, lexeme, start);
      }
      return makeSingle(TokenKind::Pipe);
    case '!':
      if (peek() == '=') {
        lexeme.push_back(advance());
        return makeToken(TokenKind::NotEqual, lexeme, start);
      }
      return makeSingle(TokenKind::LogicalNot);
    case '<':
      if (peek() == '=') {
        lexeme.push_back(advance());
        return makeToken(TokenKind::LessEqual, lexeme, start);
      }
      return makeSingle(TokenKind::Less);
    case '>':
      if (peek() == '=') {
        lexeme.push_back(advance());
        return makeToken(TokenKind::GreaterEqual, lexeme, start);
      }
      return makeSingle(TokenKind::Greater);
    case '=':
      if (peek() == '=') {
        lexeme.push_back(advance());
        return makeToken(TokenKind::EqualEqual, lexeme, start);
      }
      return makeSingle(TokenKind::Assign);
    default:
      diagnostics.warning(start, std::string("unknown character: ") + current);
      return makeToken(TokenKind::Unknown, lexeme, start);
  }
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
    case TokenKind::LongLongLiteral:
      return "long_long_literal";
    case TokenKind::FloatLiteral:
      return "float_literal";
    case TokenKind::StringLiteral:
      return "string_literal";
    case TokenKind::KeywordInt:
      return "kw_int";
    case TokenKind::KeywordFloat:
      return "kw_float";
    case TokenKind::KeywordVoid:
      return "kw_void";
    case TokenKind::KeywordBool:
      return "kw_bool";
    case TokenKind::KeywordLong:
      return "kw_long";
    case TokenKind::KeywordConst:
      return "kw_const";
    case TokenKind::KeywordReturn:
      return "kw_return";
    case TokenKind::KeywordIf:
      return "kw_if";
    case TokenKind::KeywordElse:
      return "kw_else";
    case TokenKind::KeywordFor:
      return "kw_for";
    case TokenKind::KeywordWhile:
      return "kw_while";
    case TokenKind::KeywordContinue:
      return "kw_continue";
    case TokenKind::KeywordBreak:
      return "kw_break";
    case TokenKind::KeywordSwitch:
      return "kw_switch";
    case TokenKind::KeywordCase:
      return "kw_case";
    case TokenKind::KeywordDefault:
      return "kw_default";
    case TokenKind::KeywordGoto:
      return "kw_goto";
    case TokenKind::KeywordDo:
      return "kw_do";
    case TokenKind::LParen:
      return "l_paren";
    case TokenKind::RParen:
      return "r_paren";
    case TokenKind::LBracket:
      return "l_bracket";
    case TokenKind::RBracket:
      return "r_bracket";
    case TokenKind::LBrace:
      return "l_brace";
    case TokenKind::RBrace:
      return "r_brace";
    case TokenKind::Comma:
      return "comma";
    case TokenKind::Colon:
      return "colon";
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
    case TokenKind::Less:
      return "less";
    case TokenKind::LessEqual:
      return "less_equal";
    case TokenKind::Greater:
      return "greater";
    case TokenKind::GreaterEqual:
      return "greater_equal";
    case TokenKind::EqualEqual:
      return "equal_equal";
    case TokenKind::NotEqual:
      return "not_equal";
    case TokenKind::LogicalAnd:
      return "logical_and";
    case TokenKind::LogicalOr:
      return "logical_or";
    case TokenKind::LogicalNot:
      return "logical_not";
    case TokenKind::Ampersand:
      return "ampersand";
    case TokenKind::Pipe:
      return "pipe";
    case TokenKind::Assign:
      return "assign";
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
