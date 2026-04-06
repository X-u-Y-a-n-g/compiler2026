#include "compiler/ir/builder/AstToIR.h"

#include <string>

#include "compiler/frontend/lexer/Token.h"
#include "compiler/ir/builder/IRBuilder.h"

namespace compiler::ir::builder {

namespace {

std::string inferReturnValue(const compiler::frontend::ast::TranslationUnit& unit) {
  const auto& tokens = unit.tokens();
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i].kind == compiler::frontend::lexer::TokenKind::KeywordReturn &&
        tokens[i + 1].kind == compiler::frontend::lexer::TokenKind::Number) {
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
