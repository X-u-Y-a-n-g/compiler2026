#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopCanonicalizationPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-canonicalization"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
