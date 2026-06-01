#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class TrivialBlockEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "trivial-block-elimination"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
