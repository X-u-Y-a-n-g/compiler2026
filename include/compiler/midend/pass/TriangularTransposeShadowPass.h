#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class TriangularTransposeShadowPass final : public ModulePass {
public:
  std::string name() const override { return "triangular-transpose-shadow"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
