#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class SelectSimplificationPass final : public ModulePass {
 public:
  std::string name() const override { return "select-simplify"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
