#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LCSSAPass final : public ModulePass {
 public:
  std::string name() const override { return "lcssa"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
