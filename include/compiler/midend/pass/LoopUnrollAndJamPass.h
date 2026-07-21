#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopUnrollAndJamPass final : public ModulePass {
 public:
  explicit LoopUnrollAndJamPass(int jamFactor = 10,
                                bool useAArch64VectorAccumulator = false)
      : jamFactor_(jamFactor),
        useAArch64VectorAccumulator_(useAArch64VectorAccumulator) {}

  std::string name() const override { return "loop-unroll-and-jam"; }
  void run(compiler::ir::core::Module& module) override;

 private:
  int jamFactor_;
  bool useAArch64VectorAccumulator_;
};

}  // namespace compiler::midend::pass
