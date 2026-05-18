#pragma once

#include <string>

#include "AstNode.h"
#include "LoweringContext.h"
#include "Module.h"

namespace compiler::ir::builder {

class AstToIR {
 public:
  compiler::ir::core::Module lower(const compiler::frontend::ast::TranslationUnit& unit,
                                   const std::string& sourceName,
                                   const LoweringContext& context = {}) const;
};

}  // namespace compiler::ir::builder
