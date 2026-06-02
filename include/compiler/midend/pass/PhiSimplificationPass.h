#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class PhiSimplificationPass final : public ModulePass {
 public:
  std::string name() const override { return "phi-simplification"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
