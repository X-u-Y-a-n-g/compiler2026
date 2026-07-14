#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AffineModularRecurrencePass final : public ModulePass {
 public:
  std::string name() const override { return "affine-modular-recurrence"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
