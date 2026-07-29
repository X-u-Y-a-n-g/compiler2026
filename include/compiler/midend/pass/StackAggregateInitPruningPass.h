#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class StackAggregateInitPruningPass final : public ModulePass {
public:
  std::string name() const override { return "stack-aggregate-init-pruning"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
