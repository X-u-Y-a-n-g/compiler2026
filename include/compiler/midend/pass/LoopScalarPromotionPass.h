#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopScalarPromotionPass final : public ModulePass {
public:
  std::string name() const override { return "loop-scalar-promotion"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
