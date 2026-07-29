#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LazyAffineCopyResolutionPass final : public ModulePass {
 public:
  std::string name() const override { return "lazy-affine-copy-resolution"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
