#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class GlobalDCEPass final : public ModulePass {
 public:
  std::string name() const override { return "global-dce"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
