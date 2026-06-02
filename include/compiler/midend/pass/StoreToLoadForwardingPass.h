#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class StoreToLoadForwardingPass final : public ModulePass {
 public:
  std::string name() const override { return "store-to-load-forwarding"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
