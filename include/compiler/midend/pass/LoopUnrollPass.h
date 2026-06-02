#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopUnrollPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-unroll"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
