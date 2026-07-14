#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RecursiveModularMultiplicationPass final : public ModulePass {
 public:
  std::string name() const override {
    return "recursive-modular-multiplication";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
