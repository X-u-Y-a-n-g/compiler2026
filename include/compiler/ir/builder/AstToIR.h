#pragma once

#include <string>

#include "compiler/frontend/ast/AstNode.h"
#include "compiler/ir/builder/LoweringContext.h"
#include "compiler/ir/core/Module.h"

namespace compiler::ir::builder {

class AstToIR {
 public:
  compiler::ir::core::Module lower(const compiler::frontend::ast::TranslationUnit& unit,
                                   const std::string& sourceName,
                                   const LoweringContext& context = {}) const;
};

}  // namespace compiler::ir::builder
