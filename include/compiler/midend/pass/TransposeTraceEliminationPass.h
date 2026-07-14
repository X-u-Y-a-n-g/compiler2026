#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class TransposeTraceEliminationPass final : public ModulePass {
 public:
  std::string name() const override { return "transpose-trace-elimination"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
