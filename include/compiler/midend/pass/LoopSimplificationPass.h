#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopSimplificationPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-simplify"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
