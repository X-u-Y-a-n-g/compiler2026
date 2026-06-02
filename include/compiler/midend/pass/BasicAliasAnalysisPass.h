#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class BasicAliasAnalysisPass final : public ModulePass {
 public:
  std::string name() const override { return "basic-alias-analysis"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
