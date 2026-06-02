#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class TreeShakingPass final : public ModulePass {
 public:
  std::string name() const override { return "tree-shaking"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
