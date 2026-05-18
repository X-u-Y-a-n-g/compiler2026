#include "../../../include/compiler/frontend/parser/Parser.h"

#include <memory>
#include <string>
#include <utility>

namespace compiler::frontend::parser {

using namespace compiler::frontend;

Parser::Parser(std::vector<compiler::frontend::lexer::Token> tokens)
    : tokens_(std::move(tokens)) {}

compiler::frontend::ast::TranslationUnitPtr Parser::parse(
    compiler::support::DiagnosticEngine& diagnostics) {
  if (tokens_.empty()) {
    diagnostics.error({}, "parser received an empty token stream");
    return nullptr;
  }

  auto unit = std::make_unique<ast::TranslationUnit>(tokens_.front().location, tokens_);
  while (!isAtEnd()) {
    auto decl = parseExternalDeclaration(diagnostics);
    if (decl) {
      unit->declarations.push_back(std::move(decl));
      continue;
    }
    synchronize();
  }

  return unit;
}

const Parser::Token& Parser::current() const {
  if (current_ >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[current_];
}

const Parser::Token& Parser::previous() const {
  if (current_ == 0) {
    return tokens_.front();
  }
  return tokens_[current_ - 1];
}

const Parser::Token& Parser::peek(std::size_t offset) const {
  const auto index = current_ + offset;
  if (index >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[index];
}

bool Parser::isAtEnd() const {
  return current().kind == TokenKind::EndOfFile;
}

bool Parser::check(TokenKind kind) const {
  return current().kind == kind;
}

bool Parser::match(std::initializer_list<TokenKind> kinds) {
  for (const auto kind : kinds) {
    if (check(kind)) {
      advance();
      return true;
    }
  }
  return false;
}

const Parser::Token& Parser::advance() {
  if (!isAtEnd()) {
    ++current_;
  }
  return previous();
}

const Parser::Token& Parser::expect(TokenKind kind,
                                    compiler::support::DiagnosticEngine& diagnostics,
                                    const char* message) {
  if (check(kind)) {
    return advance();
  }

  diagnostics.error(current().location,
                    std::string(message) + ", got " + lexer::toString(current().kind));
  if (!isAtEnd()) {
    return advance();
  }
  return current();
}

void Parser::synchronize() {
  if (!isAtEnd()) {
    advance();
  }

  while (!isAtEnd()) {
    if (previous().kind == TokenKind::Semicolon || previous().kind == TokenKind::RBrace) {
      return;
    }
    if (isTypeSpecifierStart(current().kind) || current().kind == TokenKind::KeywordIf ||
        current().kind == TokenKind::KeywordFor || current().kind == TokenKind::KeywordWhile ||
        current().kind == TokenKind::KeywordReturn || current().kind == TokenKind::KeywordDo ||
        current().kind == TokenKind::KeywordSwitch || current().kind == TokenKind::KeywordCase ||
        current().kind == TokenKind::KeywordDefault ||
        current().kind == TokenKind::KeywordGoto ||
        current().kind == TokenKind::KeywordBreak ||
        current().kind == TokenKind::KeywordContinue) {
      return;
    }
    advance();
  }
}

bool Parser::isTypeSpecifierStart(TokenKind kind) const {
  switch (kind) {
    case TokenKind::KeywordConst:
    case TokenKind::KeywordInt:
    case TokenKind::KeywordFloat:
    case TokenKind::KeywordVoid:
    case TokenKind::KeywordBool:
    case TokenKind::KeywordLong:
      return true;
    default:
      return false;
  }
}

Parser::TypeSpecifier Parser::parseTypeSpecifier(
    compiler::support::DiagnosticEngine& diagnostics) {
  ast::TypeSpecifier typeSpec;
  if (match({TokenKind::KeywordConst})) {
    typeSpec.isConst = true;
  }

  if (match({TokenKind::KeywordInt})) {
    typeSpec.type.kind = frontend::types::TypeKind::Int;
  } else if (match({TokenKind::KeywordFloat})) {
    typeSpec.type.kind = frontend::types::TypeKind::Float;
  } else if (match({TokenKind::KeywordVoid})) {
    typeSpec.type.kind = frontend::types::TypeKind::Void;
  } else if (match({TokenKind::KeywordBool})) {
    typeSpec.type.kind = frontend::types::TypeKind::Bool;
  } else if (match({TokenKind::KeywordLong})) {
    typeSpec.type.kind = frontend::types::TypeKind::Long;
  } else {
    diagnostics.error(current().location, "expected a type specifier");
    typeSpec.type.kind = frontend::types::TypeKind::Unknown;
  }

  return typeSpec;
}

Parser::DeclPtr Parser::parseExternalDeclaration(
    compiler::support::DiagnosticEngine& diagnostics) {
  if (!isTypeSpecifierStart(current().kind)) {
    diagnostics.error(current().location, "expected a top-level declaration");
    return nullptr;
  }

  auto type = parseTypeSpecifier(diagnostics);
  const auto& nameToken = expect(TokenKind::Identifier, diagnostics,
                                 "expected identifier after type specifier");

  if (match({TokenKind::LParen})) {
    return parseFunctionDeclaration(std::move(type), nameToken, diagnostics);
  }
  return parseVariableDeclGroup(std::move(type), nameToken, diagnostics, true);
}

std::unique_ptr<ast::VariableDecl> Parser::parseSingleVariableDeclarator(
    TypeSpecifier type, const Token& nameToken,
    compiler::support::DiagnosticEngine& diagnostics, bool isParameter) {
  auto decl = std::make_unique<ast::VariableDecl>(nameToken.location, std::move(type),
                                                  nameToken.lexeme, isParameter);

  while (match({TokenKind::LBracket})) {
    if (!check(TokenKind::RBracket)) {
      decl->dimensions.push_back(parseExpression(diagnostics));
    } else {
      decl->dimensions.push_back(nullptr);
    }
    expect(TokenKind::RBracket, diagnostics, "expected ']' after array dimension");
  }

  if (!isParameter && match({TokenKind::Assign})) {
    decl->initializer = parseInitializer(diagnostics);
  }

  return decl;
}

std::unique_ptr<ast::VariableDeclGroup> Parser::parseVariableDeclGroup(
    TypeSpecifier baseType, const Token& firstName,
    compiler::support::DiagnosticEngine& diagnostics, bool requireSemicolon, bool isParameter) {
  auto group = std::make_unique<ast::VariableDeclGroup>(firstName.location);
  group->declarations.push_back(
      parseSingleVariableDeclarator(baseType, firstName, diagnostics, isParameter));

  while (match({TokenKind::Comma})) {
    const auto& nameToken = expect(TokenKind::Identifier, diagnostics,
                                   "expected identifier after ',' in declaration");
    group->declarations.push_back(
        parseSingleVariableDeclarator(baseType, nameToken, diagnostics, isParameter));
  }

  if (requireSemicolon) {
    expect(TokenKind::Semicolon, diagnostics, "expected ';' after declaration");
  }
  return group;
}

std::unique_ptr<ast::FunctionDecl> Parser::parseFunctionDeclaration(
    TypeSpecifier returnType, const Token& nameToken,
    compiler::support::DiagnosticEngine& diagnostics) {
  auto function = std::make_unique<ast::FunctionDecl>(nameToken.location, std::move(returnType),
                                                      nameToken.lexeme);

  if (!check(TokenKind::RParen)) {
    if (check(TokenKind::KeywordVoid) && peek(1).kind == TokenKind::RParen) {
      advance();
    } else {
      do {
        auto parameterType = parseTypeSpecifier(diagnostics);
        Token parameterName = Token{TokenKind::Identifier, "", current().location};
        if (check(TokenKind::Identifier)) {
          parameterName = advance();
        }
        function->parameters.push_back(parseSingleVariableDeclarator(
            std::move(parameterType), parameterName, diagnostics, true));
      } while (match({TokenKind::Comma}));
    }
  }
  expect(TokenKind::RParen, diagnostics, "expected ')' after parameter list");

  if (check(TokenKind::LBrace)) {
    function->body = parseBlock(diagnostics);
  } else {
    expect(TokenKind::Semicolon, diagnostics,
           "expected function body or ';' after declaration");
  }
  return function;
}

std::unique_ptr<ast::BlockStmt> Parser::parseBlock(
    compiler::support::DiagnosticEngine& diagnostics) {
  const auto& brace = expect(TokenKind::LBrace, diagnostics, "expected '{' to start block");
  auto block = std::make_unique<ast::BlockStmt>(brace.location);

  while (!check(TokenKind::RBrace) && !isAtEnd()) {
    auto stmt = parseStatement(diagnostics);
    if (stmt) {
      block->statements.push_back(std::move(stmt));
    } else {
      synchronize();
    }
  }

  expect(TokenKind::RBrace, diagnostics, "expected '}' to close block");
  return block;
}

Parser::StmtPtr Parser::parseStatement(compiler::support::DiagnosticEngine& diagnostics) {
  if (check(TokenKind::LBrace)) {
    return parseBlock(diagnostics);
  }
  if (check(TokenKind::KeywordIf)) {
    return parseIfStatement(diagnostics);
  }
  if (check(TokenKind::KeywordFor)) {
    return parseForStatement(diagnostics);
  }
  if (check(TokenKind::KeywordWhile)) {
    return parseWhileStatement(diagnostics);
  }
  if (check(TokenKind::KeywordDo)) {
    return parseDoWhileStatement(diagnostics);
  }
  if (check(TokenKind::KeywordSwitch)) {
    return parseSwitchStatement(diagnostics);
  }
  if (check(TokenKind::KeywordCase)) {
    return parseCaseStatement(diagnostics);
  }
  if (check(TokenKind::KeywordDefault)) {
    return parseDefaultStatement(diagnostics);
  }
  if (check(TokenKind::KeywordReturn)) {
    return parseReturnStatement(diagnostics);
  }
  if (check(TokenKind::KeywordGoto)) {
    return parseGotoStatement(diagnostics);
  }
  if (check(TokenKind::KeywordBreak)) {
    return parseBreakStatement(diagnostics);
  }
  if (check(TokenKind::KeywordContinue)) {
    return parseContinueStatement(diagnostics);
  }
  if (isTypeSpecifierStart(current().kind)) {
    return parseDeclarationStatement(diagnostics);
  }
  if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
    return parseLabelStatement(diagnostics);
  }
  return parseExpressionStatement(diagnostics);
}

Parser::StmtPtr Parser::parseIfStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& ifToken = expect(TokenKind::KeywordIf, diagnostics, "expected 'if'");
  expect(TokenKind::LParen, diagnostics, "expected '(' after if");
  auto condition = parseExpression(diagnostics);
  expect(TokenKind::RParen, diagnostics, "expected ')' after if condition");
  auto thenBranch = parseStatement(diagnostics);
  StmtPtr elseBranch;
  if (match({TokenKind::KeywordElse})) {
    elseBranch = parseStatement(diagnostics);
  }
  return std::make_unique<ast::IfStmt>(ifToken.location, std::move(condition),
                                       std::move(thenBranch), std::move(elseBranch));
}

Parser::StmtPtr Parser::parseForStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& forToken = expect(TokenKind::KeywordFor, diagnostics, "expected 'for'");
  expect(TokenKind::LParen, diagnostics, "expected '(' after for");

  StmtPtr init;
  if (!check(TokenKind::Semicolon)) {
    if (isTypeSpecifierStart(current().kind)) {
      init = parseDeclarationStatement(diagnostics);
    } else {
      init = parseExpressionStatement(diagnostics);
    }
  } else {
    expect(TokenKind::Semicolon, diagnostics, "expected ';' in for initializer");
    init = std::make_unique<ast::ExprStmt>(forToken.location, nullptr);
  }

  ExprPtr condition;
  if (!check(TokenKind::Semicolon)) {
    condition = parseExpression(diagnostics);
  }
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after for condition");

  ExprPtr update;
  if (!check(TokenKind::RParen)) {
    update = parseExpression(diagnostics);
  }
  expect(TokenKind::RParen, diagnostics, "expected ')' after for clauses");

  auto body = parseStatement(diagnostics);
  return std::make_unique<ast::ForStmt>(forToken.location, std::move(init),
                                        std::move(condition), std::move(update),
                                        std::move(body));
}

Parser::StmtPtr Parser::parseWhileStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& whileToken = expect(TokenKind::KeywordWhile, diagnostics, "expected 'while'");
  expect(TokenKind::LParen, diagnostics, "expected '(' after while");
  auto condition = parseExpression(diagnostics);
  expect(TokenKind::RParen, diagnostics, "expected ')' after while condition");
  auto body = parseStatement(diagnostics);
  return std::make_unique<ast::WhileStmt>(whileToken.location, std::move(condition),
                                          std::move(body));
}

Parser::StmtPtr Parser::parseDoWhileStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& doToken = expect(TokenKind::KeywordDo, diagnostics, "expected 'do'");
  auto body = parseStatement(diagnostics);
  expect(TokenKind::KeywordWhile, diagnostics, "expected 'while' after do body");
  expect(TokenKind::LParen, diagnostics, "expected '(' after while");
  auto condition = parseExpression(diagnostics);
  expect(TokenKind::RParen, diagnostics, "expected ')' after do-while condition");
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after do-while");
  return std::make_unique<ast::DoWhileStmt>(doToken.location, std::move(body),
                                            std::move(condition));
}

Parser::StmtPtr Parser::parseSwitchStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& switchToken = expect(TokenKind::KeywordSwitch, diagnostics, "expected 'switch'");
  expect(TokenKind::LParen, diagnostics, "expected '(' after switch");
  auto condition = parseExpression(diagnostics);
  expect(TokenKind::RParen, diagnostics, "expected ')' after switch condition");
  expect(TokenKind::LBrace, diagnostics, "expected '{' after switch");

  auto stmt = std::make_unique<ast::SwitchStmt>(switchToken.location, std::move(condition));
  while (!check(TokenKind::RBrace) && !isAtEnd()) {
    auto clause = parseStatement(diagnostics);
    if (clause) {
      stmt->clauses.push_back(std::move(clause));
    } else {
      synchronize();
    }
  }

  expect(TokenKind::RBrace, diagnostics, "expected '}' after switch");
  return stmt;
}

Parser::StmtPtr Parser::parseCaseStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& caseToken = expect(TokenKind::KeywordCase, diagnostics, "expected 'case'");
  auto value = parseExpression(diagnostics);
  expect(TokenKind::Colon, diagnostics, "expected ':' after case value");

  auto stmt = std::make_unique<ast::CaseStmt>(caseToken.location, std::move(value));
  while (!check(TokenKind::KeywordCase) && !check(TokenKind::KeywordDefault) &&
         !check(TokenKind::RBrace) && !isAtEnd()) {
    auto nested = parseStatement(diagnostics);
    if (nested) {
      stmt->statements.push_back(std::move(nested));
    } else {
      synchronize();
    }
  }
  return stmt;
}

Parser::StmtPtr Parser::parseDefaultStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& defaultToken =
      expect(TokenKind::KeywordDefault, diagnostics, "expected 'default'");
  expect(TokenKind::Colon, diagnostics, "expected ':' after default");

  auto stmt = std::make_unique<ast::DefaultStmt>(defaultToken.location);
  while (!check(TokenKind::KeywordCase) && !check(TokenKind::KeywordDefault) &&
         !check(TokenKind::RBrace) && !isAtEnd()) {
    auto nested = parseStatement(diagnostics);
    if (nested) {
      stmt->statements.push_back(std::move(nested));
    } else {
      synchronize();
    }
  }
  return stmt;
}

Parser::StmtPtr Parser::parseReturnStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& returnToken = expect(TokenKind::KeywordReturn, diagnostics, "expected 'return'");
  ExprPtr expression;
  if (!check(TokenKind::Semicolon)) {
    expression = parseExpression(diagnostics);
  }
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after return statement");
  return std::make_unique<ast::ReturnStmt>(returnToken.location, std::move(expression));
}

Parser::StmtPtr Parser::parseGotoStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& gotoToken = expect(TokenKind::KeywordGoto, diagnostics, "expected 'goto'");
  const auto& label = expect(TokenKind::Identifier, diagnostics, "expected label after goto");
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after goto");
  return std::make_unique<ast::GotoStmt>(gotoToken.location, label.lexeme);
}

Parser::StmtPtr Parser::parseBreakStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& breakToken = expect(TokenKind::KeywordBreak, diagnostics, "expected 'break'");
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after break");
  return std::make_unique<ast::BreakStmt>(breakToken.location);
}

Parser::StmtPtr Parser::parseContinueStatement(
    compiler::support::DiagnosticEngine& diagnostics) {
  const auto& continueToken =
      expect(TokenKind::KeywordContinue, diagnostics, "expected 'continue'");
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after continue");
  return std::make_unique<ast::ContinueStmt>(continueToken.location);
}

Parser::StmtPtr Parser::parseDeclarationStatement(
    compiler::support::DiagnosticEngine& diagnostics) {
  auto type = parseTypeSpecifier(diagnostics);
  const auto& nameToken = expect(TokenKind::Identifier, diagnostics,
                                 "expected identifier after type specifier");
  auto group = parseVariableDeclGroup(std::move(type), nameToken, diagnostics, true);
  return std::make_unique<ast::DeclStmt>(nameToken.location, std::move(group));
}

Parser::StmtPtr Parser::parseExpressionStatement(
    compiler::support::DiagnosticEngine& diagnostics) {
  const auto start = current().location;
  if (match({TokenKind::Semicolon})) {
    return std::make_unique<ast::ExprStmt>(start, nullptr);
  }
  auto expression = parseExpression(diagnostics);
  expect(TokenKind::Semicolon, diagnostics, "expected ';' after expression statement");
  return std::make_unique<ast::ExprStmt>(start, std::move(expression));
}

Parser::StmtPtr Parser::parseLabelStatement(compiler::support::DiagnosticEngine& diagnostics) {
  const auto& label = expect(TokenKind::Identifier, diagnostics, "expected label identifier");
  expect(TokenKind::Colon, diagnostics, "expected ':' after label");
  auto statement = parseStatement(diagnostics);
  return std::make_unique<ast::LabelStmt>(label.location, label.lexeme, std::move(statement));
}

Parser::ExprPtr Parser::parseInitializer(compiler::support::DiagnosticEngine& diagnostics) {
  if (match({TokenKind::LBrace})) {
    auto list = std::make_unique<ast::InitListExpr>(previous().location);
    if (!check(TokenKind::RBrace)) {
      while (true) {
        list->elements.push_back(parseInitializer(diagnostics));
        if (!match({TokenKind::Comma})) {
          break;
        }
        if (check(TokenKind::RBrace)) {
          break;
        }
      }
    }
    expect(TokenKind::RBrace, diagnostics, "expected '}' after initializer list");
    return list;
  }
  return parseAssignment(diagnostics);
}

Parser::ExprPtr Parser::parseExpression(compiler::support::DiagnosticEngine& diagnostics) {
  return parseComma(diagnostics);
}

Parser::ExprPtr Parser::parseComma(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseAssignment(diagnostics);
  while (match({TokenKind::Comma})) {
    const auto op = previous().kind;
    auto rhs = parseAssignment(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseAssignment(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseLogicalOr(diagnostics);
  if (match({TokenKind::Assign})) {
    const auto op = previous().kind;
    auto rhs = parseAssignment(diagnostics);
    return std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseLogicalOr(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseLogicalAnd(diagnostics);
  while (match({TokenKind::LogicalOr})) {
    const auto op = previous().kind;
    auto rhs = parseLogicalAnd(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseLogicalAnd(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseBitwiseOr(diagnostics);
  while (match({TokenKind::LogicalAnd})) {
    const auto op = previous().kind;
    auto rhs = parseBitwiseOr(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseBitwiseOr(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseBitwiseAnd(diagnostics);
  while (match({TokenKind::Pipe})) {
    const auto op = previous().kind;
    auto rhs = parseBitwiseAnd(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseBitwiseAnd(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseEquality(diagnostics);
  while (match({TokenKind::Ampersand})) {
    const auto op = previous().kind;
    auto rhs = parseEquality(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseEquality(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseRelational(diagnostics);
  while (match({TokenKind::EqualEqual, TokenKind::NotEqual})) {
    const auto op = previous().kind;
    auto rhs = parseRelational(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseRelational(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseAdditive(diagnostics);
  while (match({TokenKind::Less, TokenKind::LessEqual, TokenKind::Greater,
                TokenKind::GreaterEqual})) {
    const auto op = previous().kind;
    auto rhs = parseAdditive(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseAdditive(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseMultiplicative(diagnostics);
  while (match({TokenKind::Plus, TokenKind::Minus})) {
    const auto op = previous().kind;
    auto rhs = parseMultiplicative(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseMultiplicative(
    compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parseUnary(diagnostics);
  while (match({TokenKind::Star, TokenKind::Slash, TokenKind::Percent})) {
    const auto op = previous().kind;
    auto rhs = parseUnary(diagnostics);
    expr = std::make_unique<ast::BinaryExpr>(expr->location(), std::move(expr), op,
                                             std::move(rhs));
  }
  return expr;
}

Parser::ExprPtr Parser::parseUnary(compiler::support::DiagnosticEngine& diagnostics) {
  if (match({TokenKind::LogicalNot, TokenKind::Plus, TokenKind::Minus, TokenKind::Ampersand,
             TokenKind::Star})) {
    const auto opToken = previous();
    auto operand = parseUnary(diagnostics);
    return std::make_unique<ast::UnaryExpr>(opToken.location, opToken.kind,
                                            std::move(operand));
  }
  return parsePostfix(diagnostics);
}

Parser::ExprPtr Parser::parsePostfix(compiler::support::DiagnosticEngine& diagnostics) {
  auto expr = parsePrimary(diagnostics);
  while (true) {
    if (match({TokenKind::LParen})) {
      auto call = std::make_unique<ast::CallExpr>(expr->location(), std::move(expr));
      if (!check(TokenKind::RParen)) {
        do {
          call->arguments.push_back(parseAssignment(diagnostics));
        } while (match({TokenKind::Comma}));
      }
      expect(TokenKind::RParen, diagnostics, "expected ')' after call arguments");
      expr = std::move(call);
      continue;
    }
    if (match({TokenKind::LBracket})) {
      auto index = parseExpression(diagnostics);
      expect(TokenKind::RBracket, diagnostics, "expected ']' after subscript expression");
      expr = std::make_unique<ast::IndexExpr>(expr->location(), std::move(expr),
                                              std::move(index));
      continue;
    }
    break;
  }
  return expr;
}

Parser::ExprPtr Parser::parsePrimary(compiler::support::DiagnosticEngine& diagnostics) {
  if (match({TokenKind::Identifier})) {
    const auto& token = previous();
    return std::make_unique<ast::IdentifierExpr>(token.location, token.lexeme);
  }
  if (match({TokenKind::Number, TokenKind::LongLongLiteral})) {
    const auto& token = previous();
    return std::make_unique<ast::IntegerLiteral>(token.location, token.lexeme,
                                                 token.kind == TokenKind::LongLongLiteral);
  }
  if (match({TokenKind::FloatLiteral})) {
    const auto& token = previous();
    return std::make_unique<ast::FloatLiteralExpr>(token.location, token.lexeme);
  }
  if (match({TokenKind::StringLiteral})) {
    const auto& token = previous();
    return std::make_unique<ast::StringLiteralExpr>(token.location, token.lexeme);
  }
  if (match({TokenKind::LParen})) {
    auto expr = parseExpression(diagnostics);
    expect(TokenKind::RParen, diagnostics, "expected ')' after grouped expression");
    return expr;
  }

  diagnostics.error(current().location,
                    std::string("expected expression, got ") + lexer::toString(current().kind));
  const auto token = current();
  if (!isAtEnd()) {
    advance();
  }
  return std::make_unique<ast::IntegerLiteral>(token.location, "0", false);
}

}  // namespace compiler::frontend::parser
