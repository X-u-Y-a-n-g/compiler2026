#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class CodeGenPreparePass final : public ModulePass {
 public:
  std::string name() const override { return "codegen-prepare"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
