#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RepeatedAccumulationEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "repeated-accumulation-elimination"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
