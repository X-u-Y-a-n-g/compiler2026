#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AggressiveDeadCodeEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "adce"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
