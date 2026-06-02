#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class SCEVAnalysisPass final : public ModulePass {
 public:
  std::string name() const override { return "scev-analysis"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
