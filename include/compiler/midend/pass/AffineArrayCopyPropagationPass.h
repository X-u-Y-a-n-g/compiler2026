#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AffineArrayCopyPropagationPass final : public ModulePass {
 public:
  std::string name() const override { return "affine-array-copy-propagation"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
