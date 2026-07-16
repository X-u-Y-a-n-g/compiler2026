#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class MonotonicContinueEliminationPass final : public ModulePass {
 public:
  std::string name() const override {
    return "monotonic-continue-elimination";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
