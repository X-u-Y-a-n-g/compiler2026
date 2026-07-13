#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class PowerOfTwoDispatchPass final : public ModulePass {
 public:
  std::string name() const override { return "power-of-two-dispatch"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
