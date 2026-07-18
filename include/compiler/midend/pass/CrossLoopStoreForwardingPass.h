#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class CrossLoopStoreForwardingPass final : public ModulePass {
 public:
  std::string name() const override { return "cross-loop-store-forwarding"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
