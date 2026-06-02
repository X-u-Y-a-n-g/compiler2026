#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ConstantFoldingPass final : public ModulePass {
 public:
  std::string name() const override { return "constant-folding"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
