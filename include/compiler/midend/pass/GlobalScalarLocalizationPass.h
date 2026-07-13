#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class GlobalScalarLocalizationPass final : public ModulePass {
 public:
  std::string name() const override { return "global-scalar-localization"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
