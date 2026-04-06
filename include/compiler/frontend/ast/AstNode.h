#pragma once

#include <memory>
#include <string>
#include <vector>

#include "compiler/frontend/lexer/Token.h"
#include "compiler/support/SourceLocation.h"

namespace compiler::frontend::ast {

class TranslationUnit;
class AstVisitor;

class AstNode {
 public:
  explicit AstNode(compiler::support::SourceLocation location);
  virtual ~AstNode() = default;

  [[nodiscard]] const compiler::support::SourceLocation& location() const;
  virtual void accept(AstVisitor& visitor) const = 0;

 private:
  compiler::support::SourceLocation location_;
};

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

class TranslationUnit final : public AstNode {
 public:
  TranslationUnit(compiler::support::SourceLocation location,
                  std::vector<compiler::frontend::lexer::Token> tokens);

  [[nodiscard]] const std::vector<compiler::frontend::lexer::Token>& tokens() const;
  void accept(AstVisitor& visitor) const override;

 private:
  std::vector<compiler::frontend::lexer::Token> tokens_;
};

class AstVisitor {
 public:
  virtual ~AstVisitor() = default;
  virtual void visit(const TranslationUnit& node) = 0;
};

using TranslationUnitPtr = std::unique_ptr<TranslationUnit>;

}  // namespace compiler::frontend::ast
