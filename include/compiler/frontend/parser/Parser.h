#pragma once

#include <initializer_list>
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
  using Token = compiler::frontend::lexer::Token;
  using TokenKind = compiler::frontend::lexer::TokenKind;
  using TypeSpecifier = compiler::frontend::ast::TypeSpecifier;
  using ExprPtr = compiler::frontend::ast::ExprPtr;
  using StmtPtr = compiler::frontend::ast::StmtPtr;
  using DeclPtr = compiler::frontend::ast::DeclPtr;

  const Token& current() const;
  const Token& previous() const;
  const Token& peek(std::size_t offset) const;
  bool isAtEnd() const;
  bool check(TokenKind kind) const;
  bool match(std::initializer_list<TokenKind> kinds);
  const Token& advance();
  const Token& expect(TokenKind kind, compiler::support::DiagnosticEngine& diagnostics,
                      const char* message);
  void synchronize();

  bool isTypeSpecifierStart(TokenKind kind) const;
  TypeSpecifier parseTypeSpecifier(compiler::support::DiagnosticEngine& diagnostics);
  DeclPtr parseExternalDeclaration(compiler::support::DiagnosticEngine& diagnostics);
  std::unique_ptr<compiler::frontend::ast::VariableDeclGroup> parseVariableDeclGroup(
      TypeSpecifier baseType, const Token& firstName,
      compiler::support::DiagnosticEngine& diagnostics, bool requireSemicolon,
      bool isParameter = false);
  std::unique_ptr<compiler::frontend::ast::VariableDecl> parseSingleVariableDeclarator(
      TypeSpecifier type, const Token& nameToken,
      compiler::support::DiagnosticEngine& diagnostics, bool isParameter);
  std::unique_ptr<compiler::frontend::ast::FunctionDecl> parseFunctionDeclaration(
      TypeSpecifier returnType, const Token& nameToken,
      compiler::support::DiagnosticEngine& diagnostics);
  std::unique_ptr<compiler::frontend::ast::BlockStmt> parseBlock(
      compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseIfStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseForStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseWhileStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseDoWhileStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseSwitchStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseCaseStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseDefaultStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseReturnStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseGotoStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseBreakStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseContinueStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseDeclarationStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseExpressionStatement(compiler::support::DiagnosticEngine& diagnostics);
  StmtPtr parseLabelStatement(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseInitializer(compiler::support::DiagnosticEngine& diagnostics);

  ExprPtr parseExpression(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseComma(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseAssignment(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseLogicalOr(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseLogicalAnd(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseBitwiseOr(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseBitwiseAnd(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseEquality(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseRelational(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseAdditive(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseMultiplicative(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parseUnary(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parsePostfix(compiler::support::DiagnosticEngine& diagnostics);
  ExprPtr parsePrimary(compiler::support::DiagnosticEngine& diagnostics);

  std::vector<Token> tokens_;
  std::size_t current_ = 0;
};

}  // namespace compiler::frontend::parser
