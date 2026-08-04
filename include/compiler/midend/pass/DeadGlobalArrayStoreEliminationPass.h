#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class DeadGlobalArrayStoreEliminationPass final : public ModulePass {
 public:
  std::string name() const override {
    return "dead-global-array-store-elimination";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
