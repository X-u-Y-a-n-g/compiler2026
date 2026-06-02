#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class FunctionInliningPass final : public ModulePass {
 public:
  std::string name() const override { return "function-inlining"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
