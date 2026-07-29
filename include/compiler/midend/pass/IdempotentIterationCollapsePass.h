#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class IdempotentIterationCollapsePass final : public ModulePass {
public:
  std::string name() const override { return "idempotent-iteration-collapse"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
