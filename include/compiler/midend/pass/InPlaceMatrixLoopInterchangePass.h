#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class InPlaceMatrixLoopInterchangePass final : public ModulePass {
public:
  std::string name() const override {
    return "in-place-matrix-loop-interchange";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
