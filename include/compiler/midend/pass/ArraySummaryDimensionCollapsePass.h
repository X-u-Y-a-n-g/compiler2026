#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ArraySummaryDimensionCollapsePass final : public ModulePass {
public:
  std::string name() const override {
    return "array-summary-dimension-collapse";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
