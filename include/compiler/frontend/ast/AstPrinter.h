#pragma once

#include <string>

#include "compiler/frontend/ast/AstNode.h"

namespace compiler::frontend::ast {

class AstPrinter {
 public:
  std::string print(const TranslationUnit& unit);

 private:
 void printDecl(const Decl& decl, int indent);
 void printStmt(const Stmt& stmt, int indent);
 void printExpr(const Expr& expr, int indent);
  void printInitializer(const Expr& expr, int indent);
  void printVariableDecl(const VariableDecl& decl, int indent);
  void line(int indent, const std::string& text);

  std::string output_;
};

}  // namespace compiler::frontend::ast
