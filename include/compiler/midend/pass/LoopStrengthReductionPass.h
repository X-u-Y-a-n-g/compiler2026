#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopStrengthReductionPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-strength-reduce"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
