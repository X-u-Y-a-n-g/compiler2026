#include "compiler/ir/builder/AstToIR.h"

#include <optional>
#include <string>

#include "compiler/frontend/ast/AstNode.h"
#include "compiler/frontend/lexer/Token.h"
#include "compiler/ir/builder/IRBuilder.h"

namespace compiler::ir::builder {

namespace {

std::optional<std::string> evaluateIntegerExpr(const compiler::frontend::ast::Expr* expr) {
  using namespace compiler::frontend::ast;
  if (expr == nullptr) {
    return std::nullopt;
  }
  if (const auto* integer = dynamic_cast<const IntegerLiteral*>(expr)) {
    return integer->value;
  }
  if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
    const auto operand = evaluateIntegerExpr(unary->operand.get());
    if (!operand.has_value()) {
      return std::nullopt;
    }
    if (unary->op == compiler::frontend::lexer::TokenKind::Minus) {
      return "-" + *operand;
    }
    if (unary->op == compiler::frontend::lexer::TokenKind::Plus) {
      return *operand;
    }
  }
  return std::nullopt;
}

std::optional<std::string> findReturnValueInStmt(const compiler::frontend::ast::Stmt* stmt) {
  using namespace compiler::frontend::ast;
  if (stmt == nullptr) {
    return std::nullopt;
  }
  if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
    for (const auto& nested : block->statements) {
      if (const auto value = findReturnValueInStmt(nested.get()); value.has_value()) {
        return value;
      }
    }
    return std::nullopt;
  }
  if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
    return evaluateIntegerExpr(ret->expression.get());
  }
  if (const auto* ifStmt = dynamic_cast<const IfStmt*>(stmt)) {
    if (const auto value = findReturnValueInStmt(ifStmt->thenBranch.get()); value.has_value()) {
      return value;
    }
    return findReturnValueInStmt(ifStmt->elseBranch.get());
  }
  if (const auto* forStmt = dynamic_cast<const ForStmt*>(stmt)) {
    return findReturnValueInStmt(forStmt->body.get());
  }
  if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(stmt)) {
    return findReturnValueInStmt(whileStmt->body.get());
  }
  if (const auto* doWhileStmt = dynamic_cast<const DoWhileStmt*>(stmt)) {
    return findReturnValueInStmt(doWhileStmt->body.get());
  }
  if (const auto* label = dynamic_cast<const LabelStmt*>(stmt)) {
    return findReturnValueInStmt(label->statement.get());
  }
  if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(stmt)) {
    for (const auto& nested : caseStmt->statements) {
      if (const auto value = findReturnValueInStmt(nested.get()); value.has_value()) {
        return value;
      }
    }
  }
  if (const auto* defaultStmt = dynamic_cast<const DefaultStmt*>(stmt)) {
    for (const auto& nested : defaultStmt->statements) {
      if (const auto value = findReturnValueInStmt(nested.get()); value.has_value()) {
        return value;
      }
    }
  }
  if (const auto* switchStmt = dynamic_cast<const SwitchStmt*>(stmt)) {
    for (const auto& clause : switchStmt->clauses) {
      if (const auto value = findReturnValueInStmt(clause.get()); value.has_value()) {
        return value;
      }
    }
  }
  return std::nullopt;
}

std::string inferReturnValue(const compiler::frontend::ast::TranslationUnit& unit) {
  using namespace compiler::frontend::ast;
  for (const auto& decl : unit.declarations) {
    const auto* function = dynamic_cast<const FunctionDecl*>(decl.get());
    if (function == nullptr || function->name != "main" || function->body == nullptr) {
      continue;
    }
    if (const auto value = findReturnValueInStmt(function->body.get()); value.has_value()) {
      return *value;
    }
  }

  const auto& tokens = unit.tokens;
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i].kind == compiler::frontend::lexer::TokenKind::KeywordReturn &&
        (tokens[i + 1].kind == compiler::frontend::lexer::TokenKind::Number ||
         tokens[i + 1].kind == compiler::frontend::lexer::TokenKind::LongLongLiteral)) {
      return tokens[i + 1].lexeme;
    }
  }
  return "0";
}

}  // namespace

compiler::ir::core::Module AstToIR::lower(const compiler::frontend::ast::TranslationUnit& unit,
                                          const std::string& sourceName,
                                          const LoweringContext& context) const {
  compiler::ir::core::Module module(sourceName);
  auto& function = module.addFunction(compiler::ir::core::Function(context.entryFunctionName));
  IRBuilder builder(function);
  auto& entry = builder.createBlock("entry");
  builder.emit(entry, "ret", {inferReturnValue(unit)});
  return module;
}

}  // namespace compiler::ir::builder
