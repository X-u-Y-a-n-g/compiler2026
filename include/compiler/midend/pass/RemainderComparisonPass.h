#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RemainderComparisonPass final : public ModulePass {
 public:
  std::string name() const override { return "remainder-comparison"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
