#pragma once

#include <string>

#include "compiler/frontend/ast/AstNode.h"

namespace compiler::frontend::ast {

class AstPrinter : public AstVisitor {
 public:
  std::string print(const TranslationUnit& unit);
  void visit(const TranslationUnit& node) override;

 private:
  std::string output_;
};

}  // namespace compiler::frontend::ast
