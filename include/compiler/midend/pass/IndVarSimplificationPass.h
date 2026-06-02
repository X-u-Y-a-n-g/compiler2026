#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class IndVarSimplificationPass final : public ModulePass {
 public:
  std::string name() const override { return "indvar-simplify"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
