#pragma once

#include <string>

namespace compiler::ir::builder {

struct LoweringContext {
  std::string entryFunctionName = "main";
};

}  // namespace compiler::ir::builder
