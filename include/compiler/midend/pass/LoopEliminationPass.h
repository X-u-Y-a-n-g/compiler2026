#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-elimination"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
