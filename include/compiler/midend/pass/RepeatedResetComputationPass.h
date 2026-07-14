#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RepeatedResetComputationPass final : public ModulePass {
 public:
  std::string name() const override { return "repeated-reset-computation"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
