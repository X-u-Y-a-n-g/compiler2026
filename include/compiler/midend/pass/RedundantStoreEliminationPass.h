#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RedundantStoreEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "redundant-store-elimination"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
