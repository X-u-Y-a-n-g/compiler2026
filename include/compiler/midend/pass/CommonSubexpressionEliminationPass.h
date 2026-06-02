#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class CommonSubexpressionEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "common-subexpression-elimination"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
