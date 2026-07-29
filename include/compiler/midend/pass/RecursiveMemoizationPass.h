#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RecursiveMemoizationPass final : public ModulePass {
public:
  std::string name() const override { return "recursive-memoization"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
