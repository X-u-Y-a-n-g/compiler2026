#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopAnalysisPass final : public ModulePass {
 public:
  std::string name() const override { return "loop-analysis"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
