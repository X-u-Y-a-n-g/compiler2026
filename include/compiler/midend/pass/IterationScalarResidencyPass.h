#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class IterationScalarResidencyPass final : public ModulePass {
public:
  std::string name() const override { return "iteration-scalar-residency"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
