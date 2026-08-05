#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

// Removes redundant materialization of a uniformly initialized 3-D array when
// lexicographic stencil traversal proves future cells still hold the seed.
class UniformInitializedStencilElisionPass final : public ModulePass {
public:
  std::string name() const override {
    return "uniform-initialized-stencil-elision";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
