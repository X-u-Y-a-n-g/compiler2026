#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class DivRemPairStrengthReductionPass final : public ModulePass {
 public:
  std::string name() const override {
    return "div-rem-pair-strength-reduction";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
