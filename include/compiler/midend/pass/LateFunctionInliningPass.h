#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LateFunctionInliningPass final : public ModulePass {
public:
  std::string name() const override { return "late-function-inlining"; }
  bool run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
