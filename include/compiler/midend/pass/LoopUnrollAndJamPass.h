#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopUnrollAndJamPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-unroll-and-jam"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
