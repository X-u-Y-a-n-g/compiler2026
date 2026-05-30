#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class UnreachableBlockEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "unreachable-block-elimination"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
