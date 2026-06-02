#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class FunctionSpecializationPass final : public ModulePass {
 public:
  std::string name() const override { return "function-specialization"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
