#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ConstantFoldingPass final : public ModulePass {
 public:
  explicit ConstantFoldingPass(bool foldAllIntegerConstants = false)
      : foldAllIntegerConstants_(foldAllIntegerConstants) {}
  std::string name() const override { return "constant-folding"; }
  void run(compiler::ir::core::Module& module) override;

 private:
  bool foldAllIntegerConstants_ = false;
};

}  // namespace compiler::midend::pass
