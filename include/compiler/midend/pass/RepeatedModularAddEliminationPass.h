#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RepeatedModularAddEliminationPass final : public ModulePass {
public:
  std::string name() const override {
    return "repeated-modular-add-elimination";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
