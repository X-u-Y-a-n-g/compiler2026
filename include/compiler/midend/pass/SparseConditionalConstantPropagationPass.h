#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class SparseConditionalConstantPropagationPass final : public ModulePass {
 public:
  std::string name() const override { return "sccp"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
