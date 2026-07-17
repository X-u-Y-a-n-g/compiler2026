#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class MonotonicLoopBoundPass final : public ModulePass {
 public:
  std::string name() const override { return "monotonic-loop-bound"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
