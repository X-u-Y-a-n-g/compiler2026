#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class GlobalConstPropagationPass final : public ModulePass {
 public:
  std::string name() const override { return "global-const-prop"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
