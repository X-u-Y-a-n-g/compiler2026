#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Token.h"
#include "Type.h"
#include "SourceLocation.h"

namespace compiler::frontend::ast {

struct TypeSpecifier {
  bool isConst = false;
  compiler::frontend::types::Type type;

  [[nodiscard]] std::string toString() const {
    return isConst ? "const " + type.toString() : type.toString();
  }
};

class AstNode {
 public:
  explicit AstNode(compiler::support::SourceLocation location) : location_(location) {}
  virtual ~AstNode() = default;

  [[nodiscard]] const compiler::support::SourceLocation& location() const { return location_; }

 private:
  compiler::support::SourceLocation location_;
};

class Expr;
class Stmt;
class Decl;
class BlockStmt;
class VariableDecl;
class VariableDeclGroup;
class FunctionDecl;
class InitListExpr;
class IdentifierExpr;
class IntegerLiteral;
class FloatLiteralExpr;
class StringLiteralExpr;
class UnaryExpr;
class BinaryExpr;
class CallExpr;
class IndexExpr;
class ExprStmt;
class DeclStmt;
class ReturnStmt;
class IfStmt;
class ForStmt;
class WhileStmt;
class DoWhileStmt;
class BreakStmt;
class ContinueStmt;
class GotoStmt;
class LabelStmt;
class CaseStmt;
class DefaultStmt;
class SwitchStmt;
class TranslationUnit;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

class Expr : public AstNode {
 public:
  using AstNode::AstNode;
};

class Stmt : public AstNode {
 public:
  using AstNode::AstNode;
};

class Decl : public AstNode {
 public:
  using AstNode::AstNode;
};

class IdentifierExpr final : public Expr {
 public:
  IdentifierExpr(compiler::support::SourceLocation location, std::string name)
      : Expr(location), name(std::move(name)) {}

  std::string name;
};

class IntegerLiteral final : public Expr {
 public:
  IntegerLiteral(compiler::support::SourceLocation location, std::string value, bool isLongLong)
      : Expr(location), value(std::move(value)), isLongLong(isLongLong) {}

  std::string value;
  bool isLongLong = false;
};

class FloatLiteralExpr final : public Expr {
 public:
  FloatLiteralExpr(compiler::support::SourceLocation location, std::string value)
      : Expr(location), value(std::move(value)) {}

  std::string value;
};

class StringLiteralExpr final : public Expr {
 public:
  StringLiteralExpr(compiler::support::SourceLocation location, std::string value)
      : Expr(location), value(std::move(value)) {}

  std::string value;
};

class UnaryExpr final : public Expr {
 public:
  UnaryExpr(compiler::support::SourceLocation location,
            compiler::frontend::lexer::TokenKind op, ExprPtr operand)
      : Expr(location), op(op), operand(std::move(operand)) {}

  compiler::frontend::lexer::TokenKind op;
  ExprPtr operand;
};

class BinaryExpr final : public Expr {
 public:
  BinaryExpr(compiler::support::SourceLocation location, ExprPtr lhs,
             compiler::frontend::lexer::TokenKind op, ExprPtr rhs)
      : Expr(location), lhs(std::move(lhs)), op(op), rhs(std::move(rhs)) {}

  ExprPtr lhs;
  compiler::frontend::lexer::TokenKind op;
  ExprPtr rhs;
};

class CallExpr final : public Expr {
 public:
  CallExpr(compiler::support::SourceLocation location, ExprPtr callee)
      : Expr(location), callee(std::move(callee)) {}

  ExprPtr callee;
  std::vector<ExprPtr> arguments;
};

class IndexExpr final : public Expr {
 public:
  IndexExpr(compiler::support::SourceLocation location, ExprPtr base, ExprPtr index)
      : Expr(location), base(std::move(base)), index(std::move(index)) {}

  ExprPtr base;
  ExprPtr index;
};

class InitListExpr final : public Expr {
 public:
  explicit InitListExpr(compiler::support::SourceLocation location) : Expr(location) {}

  std::vector<ExprPtr> elements;
};

struct NormalizedInitializerSlot {
  const Expr* expression = nullptr;

  [[nodiscard]] bool isImplicitZero() const { return expression == nullptr; }
};

struct NormalizedInitializerData {
  bool available = false;
  std::vector<NormalizedInitializerSlot> slots;

  void clear() {
    available = false;
    slots.clear();
  }
};

class VariableDecl final : public AstNode {
 public:
  VariableDecl(compiler::support::SourceLocation location, TypeSpecifier type, std::string name,
               bool isParameter = false)
      : AstNode(location), type(std::move(type)), name(std::move(name)), isParameter(isParameter) {}

  TypeSpecifier type;
  std::string name;
  std::vector<ExprPtr> dimensions;
  ExprPtr initializer;
  bool isParameter = false;
  // Analysis cache filled after type checking: flattened initializer slots, with nullptr meaning
  // implicit zero-fill.
  mutable NormalizedInitializerData normalizedInitializer;
};

class VariableDeclGroup final : public Decl {
 public:
  explicit VariableDeclGroup(compiler::support::SourceLocation location) : Decl(location) {}

  std::vector<std::unique_ptr<VariableDecl>> declarations;
};

class BlockStmt final : public Stmt {
 public:
  explicit BlockStmt(compiler::support::SourceLocation location) : Stmt(location) {}

  std::vector<StmtPtr> statements;
};

class FunctionDecl final : public Decl {
 public:
  FunctionDecl(compiler::support::SourceLocation location, TypeSpecifier returnType,
               std::string name)
      : Decl(location), returnType(std::move(returnType)), name(std::move(name)) {}

  TypeSpecifier returnType;
  std::string name;
  std::vector<std::unique_ptr<VariableDecl>> parameters;
  std::unique_ptr<BlockStmt> body;
};

class ExprStmt final : public Stmt {
 public:
  ExprStmt(compiler::support::SourceLocation location, ExprPtr expression)
      : Stmt(location), expression(std::move(expression)) {}

  ExprPtr expression;
};

class DeclStmt final : public Stmt {
 public:
  DeclStmt(compiler::support::SourceLocation location,
           std::unique_ptr<VariableDeclGroup> declaration)
      : Stmt(location), declaration(std::move(declaration)) {}

  std::unique_ptr<VariableDeclGroup> declaration;
};

class ReturnStmt final : public Stmt {
 public:
  ReturnStmt(compiler::support::SourceLocation location, ExprPtr expression)
      : Stmt(location), expression(std::move(expression)) {}

  ExprPtr expression;
};

class IfStmt final : public Stmt {
 public:
  IfStmt(compiler::support::SourceLocation location, ExprPtr condition, StmtPtr thenBranch,
         StmtPtr elseBranch)
      : Stmt(location),
        condition(std::move(condition)),
        thenBranch(std::move(thenBranch)),
        elseBranch(std::move(elseBranch)) {}

  ExprPtr condition;
  StmtPtr thenBranch;
  StmtPtr elseBranch;
};

class ForStmt final : public Stmt {
 public:
  ForStmt(compiler::support::SourceLocation location, StmtPtr init, ExprPtr condition,
          ExprPtr update, StmtPtr body)
      : Stmt(location),
        init(std::move(init)),
        condition(std::move(condition)),
        update(std::move(update)),
        body(std::move(body)) {}

  StmtPtr init;
  ExprPtr condition;
  ExprPtr update;
  StmtPtr body;
};

class WhileStmt final : public Stmt {
 public:
  WhileStmt(compiler::support::SourceLocation location, ExprPtr condition, StmtPtr body)
      : Stmt(location), condition(std::move(condition)), body(std::move(body)) {}

  ExprPtr condition;
  StmtPtr body;
};

class DoWhileStmt final : public Stmt {
 public:
  DoWhileStmt(compiler::support::SourceLocation location, StmtPtr body, ExprPtr condition)
      : Stmt(location), body(std::move(body)), condition(std::move(condition)) {}

  StmtPtr body;
  ExprPtr condition;
};

class BreakStmt final : public Stmt {
 public:
  using Stmt::Stmt;
};

class ContinueStmt final : public Stmt {
 public:
  using Stmt::Stmt;
};

class GotoStmt final : public Stmt {
 public:
  GotoStmt(compiler::support::SourceLocation location, std::string label)
      : Stmt(location), label(std::move(label)) {}

  std::string label;
};

class LabelStmt final : public Stmt {
 public:
  LabelStmt(compiler::support::SourceLocation location, std::string label, StmtPtr statement)
      : Stmt(location), label(std::move(label)), statement(std::move(statement)) {}

  std::string label;
  StmtPtr statement;
};

class CaseStmt final : public Stmt {
 public:
  CaseStmt(compiler::support::SourceLocation location, ExprPtr value)
      : Stmt(location), value(std::move(value)) {}

  ExprPtr value;
  std::vector<StmtPtr> statements;
};

class DefaultStmt final : public Stmt {
 public:
  explicit DefaultStmt(compiler::support::SourceLocation location) : Stmt(location) {}

  std::vector<StmtPtr> statements;
};

class SwitchStmt final : public Stmt {
 public:
  SwitchStmt(compiler::support::SourceLocation location, ExprPtr condition)
      : Stmt(location), condition(std::move(condition)) {}

  ExprPtr condition;
  std::vector<StmtPtr> clauses;
};

class TranslationUnit final : public AstNode {
 public:
  TranslationUnit(compiler::support::SourceLocation location,
                  std::vector<compiler::frontend::lexer::Token> tokens)
      : AstNode(location), tokens(std::move(tokens)) {}

  std::vector<compiler::frontend::lexer::Token> tokens;
  std::vector<DeclPtr> declarations;
};

using TranslationUnitPtr = std::unique_ptr<TranslationUnit>;

}  // namespace compiler::frontend::ast
