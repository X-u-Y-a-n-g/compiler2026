#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class DeadPrefixIterationEliminationPass final : public ModulePass {
public:
  std::string name() const override {
    return "dead-prefix-iteration-elimination";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
