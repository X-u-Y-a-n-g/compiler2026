#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopInvariantGuardHoistingPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-invariant-guard-hoisting"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
