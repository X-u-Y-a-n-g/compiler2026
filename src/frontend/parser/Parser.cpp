#include "compiler/frontend/parser/Parser.h"

#include <utility>

namespace compiler::frontend::ast {

AstNode::AstNode(compiler::support::SourceLocation location) : location_(location) {}

const compiler::support::SourceLocation& AstNode::location() const {
  return location_;
}

TranslationUnit::TranslationUnit(compiler::support::SourceLocation location,
                                 std::vector<compiler::frontend::lexer::Token> tokens)
    : AstNode(location), tokens_(std::move(tokens)) {}

const std::vector<compiler::frontend::lexer::Token>& TranslationUnit::tokens() const {
  return tokens_;
}

void TranslationUnit::accept(AstVisitor& visitor) const {
  visitor.visit(*this);
}

}  // namespace compiler::frontend::ast

namespace compiler::frontend::parser {

Parser::Parser(std::vector<compiler::frontend::lexer::Token> tokens)
    : tokens_(std::move(tokens)) {}

compiler::frontend::ast::TranslationUnitPtr Parser::parse(
    compiler::support::DiagnosticEngine& diagnostics) {
  if (tokens_.empty()) {
    diagnostics.error({}, "parser received an empty token stream");
    return nullptr;
  }

  return std::make_unique<compiler::frontend::ast::TranslationUnit>(tokens_.front().location,
                                                                    tokens_);
}

}  // namespace compiler::frontend::parser
