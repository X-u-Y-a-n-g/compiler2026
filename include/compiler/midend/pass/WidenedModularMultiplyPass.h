#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class WidenedModularMultiplyPass final : public ModulePass {
 public:
  std::string name() const override { return "widened-modular-multiply"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
