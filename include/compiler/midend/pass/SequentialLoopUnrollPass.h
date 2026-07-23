#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class SequentialLoopUnrollPass final : public ModulePass {
 public:
  explicit SequentialLoopUnrollPass(int factor = 4) : factor_(factor) {}

  std::string name() const override { return "sequential-loop-unroll"; }
  void run(compiler::ir::core::Module& module) override;

 private:
  int factor_;
};

}  // namespace compiler::midend::pass
