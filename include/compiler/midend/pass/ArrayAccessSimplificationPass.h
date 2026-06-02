#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ArrayAccessSimplificationPass final : public ModulePass {
 public:
  std::string name() const override { return "array-access-simplify"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
