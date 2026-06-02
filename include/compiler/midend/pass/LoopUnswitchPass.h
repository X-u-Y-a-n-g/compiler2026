#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopUnswitchPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-unswitch"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
