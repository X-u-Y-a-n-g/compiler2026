#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopInvariantCodeMotionPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-invariant-code-motion"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
