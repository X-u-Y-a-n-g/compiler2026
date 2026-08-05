#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ModularAffineCanonicalizationPass final : public ModulePass {
 public:
  std::string name() const override {
    return "modular-affine-canonicalization";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
