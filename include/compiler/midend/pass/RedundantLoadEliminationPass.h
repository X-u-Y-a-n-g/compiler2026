#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RedundantLoadEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "redundant-load-elimination"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
