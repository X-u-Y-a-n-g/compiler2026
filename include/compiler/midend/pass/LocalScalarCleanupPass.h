#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LocalScalarCleanupPass final : public ModulePass {
 public:
  std::string name() const override { return "local-scalar-cleanup"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
