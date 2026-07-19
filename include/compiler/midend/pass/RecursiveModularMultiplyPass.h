#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RecursiveModularMultiplyPass final : public ModulePass {
 public:
  std::string name() const override { return "recursive-modular-multiply"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
