#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopCarriedStoreToLoadForwardingPass final : public ModulePass {
public:
  std::string name() const override {
    return "loop-carried-store-to-load-forwarding";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
