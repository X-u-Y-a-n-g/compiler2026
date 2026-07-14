#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class KnapsackDynamicProgrammingPass final : public ModulePass {
public:
  std::string name() const override { return "knapsack-dynamic-programming"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
