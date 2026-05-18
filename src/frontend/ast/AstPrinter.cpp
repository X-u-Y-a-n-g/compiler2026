#include "AstPrinter.h"

#include <sstream>
#include <string>

#include "Token.h"

namespace compiler::frontend::ast {

using compiler::frontend::lexer::toString;

std::string AstPrinter::print(const TranslationUnit& unit) {
  output_.clear();
  line(0, "TranslationUnit");
  line(1, "token_count: " + std::to_string(unit.tokens.size()));
  line(1, "declaration_count: " + std::to_string(unit.declarations.size()));
  for (const auto& decl : unit.declarations) {
    printDecl(*decl, 1);
  }
  return output_;
}

void AstPrinter::printDecl(const Decl& decl, int indent) {
  if (const auto* function = dynamic_cast<const FunctionDecl*>(&decl)) {
    line(indent, "FunctionDecl " + function->returnType.toString() + " " + function->name);
    line(indent + 1, "param_count: " + std::to_string(function->parameters.size()));
    for (const auto& parameter : function->parameters) {
      printVariableDecl(*parameter, indent + 2);
    }
    if (function->body) {
      printStmt(*function->body, indent + 1);
    }
    return;
  }

  if (const auto* group = dynamic_cast<const VariableDeclGroup*>(&decl)) {
    line(indent, "VariableDeclGroup");
    for (const auto& variable : group->declarations) {
      printVariableDecl(*variable, indent + 1);
    }
    return;
  }

  line(indent, "UnknownDecl");
}

void AstPrinter::printStmt(const Stmt& stmt, int indent) {
  if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
    line(indent, "BlockStmt");
    for (const auto& inner : block->statements) {
      printStmt(*inner, indent + 1);
    }
    return;
  }
  if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
    line(indent, "DeclStmt");
    if (declStmt->declaration) {
      printDecl(*declStmt->declaration, indent + 1);
    }
    return;
  }
  if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
    line(indent, "ExprStmt");
    if (exprStmt->expression) {
      printExpr(*exprStmt->expression, indent + 1);
    }
    return;
  }
  if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
    line(indent, "ReturnStmt");
    if (returnStmt->expression) {
      printExpr(*returnStmt->expression, indent + 1);
    }
    return;
  }
  if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
    line(indent, "IfStmt");
    line(indent + 1, "Condition");
    printExpr(*ifStmt->condition, indent + 2);
    line(indent + 1, "Then");
    printStmt(*ifStmt->thenBranch, indent + 2);
    if (ifStmt->elseBranch) {
      line(indent + 1, "Else");
      printStmt(*ifStmt->elseBranch, indent + 2);
    }
    return;
  }
  if (const auto* forStmt = dynamic_cast<const ForStmt*>(&stmt)) {
    line(indent, "ForStmt");
    if (forStmt->init) {
      line(indent + 1, "Init");
      printStmt(*forStmt->init, indent + 2);
    }
    if (forStmt->condition) {
      line(indent + 1, "Condition");
      printExpr(*forStmt->condition, indent + 2);
    }
    if (forStmt->update) {
      line(indent + 1, "Update");
      printExpr(*forStmt->update, indent + 2);
    }
    if (forStmt->body) {
      line(indent + 1, "Body");
      printStmt(*forStmt->body, indent + 2);
    }
    return;
  }
  if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
    line(indent, "WhileStmt");
    printExpr(*whileStmt->condition, indent + 1);
    printStmt(*whileStmt->body, indent + 1);
    return;
  }
  if (const auto* doWhileStmt = dynamic_cast<const DoWhileStmt*>(&stmt)) {
    line(indent, "DoWhileStmt");
    printStmt(*doWhileStmt->body, indent + 1);
    printExpr(*doWhileStmt->condition, indent + 1);
    return;
  }
  if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
    line(indent, "BreakStmt");
    return;
  }
  if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
    line(indent, "ContinueStmt");
    return;
  }
  if (const auto* gotoStmt = dynamic_cast<const GotoStmt*>(&stmt)) {
    line(indent, "GotoStmt " + gotoStmt->label);
    return;
  }
  if (const auto* labelStmt = dynamic_cast<const LabelStmt*>(&stmt)) {
    line(indent, "LabelStmt " + labelStmt->label);
    if (labelStmt->statement) {
      printStmt(*labelStmt->statement, indent + 1);
    }
    return;
  }
  if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(&stmt)) {
    line(indent, "CaseStmt");
    printExpr(*caseStmt->value, indent + 1);
    for (const auto& nested : caseStmt->statements) {
      printStmt(*nested, indent + 1);
    }
    return;
  }
  if (const auto* defaultStmt = dynamic_cast<const DefaultStmt*>(&stmt)) {
    line(indent, "DefaultStmt");
    for (const auto& nested : defaultStmt->statements) {
      printStmt(*nested, indent + 1);
    }
    return;
  }
  if (const auto* switchStmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
    line(indent, "SwitchStmt");
    line(indent + 1, "Condition");
    printExpr(*switchStmt->condition, indent + 2);
    for (const auto& clause : switchStmt->clauses) {
      printStmt(*clause, indent + 1);
    }
    return;
  }

  line(indent, "UnknownStmt");
}

void AstPrinter::printExpr(const Expr& expr, int indent) {
  if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expr)) {
    line(indent, "IdentifierExpr " + identifier->name);
    return;
  }
  if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expr)) {
    line(indent, std::string(integer->isLongLong ? "LongLongLiteral " : "IntegerLiteral ") +
                     integer->value);
    return;
  }
  if (const auto* floatLiteral = dynamic_cast<const FloatLiteralExpr*>(&expr)) {
    line(indent, "FloatLiteral " + floatLiteral->value);
    return;
  }
  if (const auto* stringLiteral = dynamic_cast<const StringLiteralExpr*>(&expr)) {
    line(indent, "StringLiteral \"" + stringLiteral->value + "\"");
    return;
  }
  if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
    line(indent, "UnaryExpr " + toString(unary->op));
    if (unary->operand) {
      printExpr(*unary->operand, indent + 1);
    }
    return;
  }
  if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
    line(indent, "BinaryExpr " + toString(binary->op));
    printExpr(*binary->lhs, indent + 1);
    printExpr(*binary->rhs, indent + 1);
    return;
  }
  if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
    line(indent, "CallExpr");
    printExpr(*call->callee, indent + 1);
    for (const auto& argument : call->arguments) {
      printExpr(*argument, indent + 1);
    }
    return;
  }
  if (const auto* index = dynamic_cast<const IndexExpr*>(&expr)) {
    line(indent, "IndexExpr");
    printExpr(*index->base, indent + 1);
      printExpr(*index->index, indent + 1);
    return;
  }
  if (const auto* initList = dynamic_cast<const InitListExpr*>(&expr)) {
    line(indent, "InitListExpr");
    for (const auto& element : initList->elements) {
      printInitializer(*element, indent + 1);
    }
    return;
  }

  line(indent, "UnknownExpr");
}

void AstPrinter::printInitializer(const Expr& expr, int indent) {
  if (dynamic_cast<const InitListExpr*>(&expr) != nullptr) {
    printExpr(expr, indent);
    return;
  }
  line(indent, "InitElement");
  printExpr(expr, indent + 1);
}

void AstPrinter::printVariableDecl(const VariableDecl& decl, int indent) {
  line(indent, "VariableDecl " + decl.type.toString() + " " + decl.name +
                   (decl.isParameter ? " [param]" : ""));
  for (const auto& dimension : decl.dimensions) {
    line(indent + 1, "Dimension");
    if (dimension) {
      printExpr(*dimension, indent + 2);
    }
  }
  if (decl.initializer) {
    line(indent + 1, "Initializer");
    printInitializer(*decl.initializer, indent + 2);
  }
}

void AstPrinter::line(int indent, const std::string& text) {
  output_.append(static_cast<std::size_t>(indent) * 2, ' ');
  output_ += text;
  output_ += '\n';
}

}  // namespace compiler::frontend::ast
