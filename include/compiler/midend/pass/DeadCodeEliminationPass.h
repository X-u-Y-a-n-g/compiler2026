#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class DeadCodeEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "dead-code-elimination"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
