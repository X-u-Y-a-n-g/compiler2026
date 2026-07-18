#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ArrayDimensionLayoutPass final : public ModulePass {
 public:
  std::string name() const override { return "array-dimension-layout"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
