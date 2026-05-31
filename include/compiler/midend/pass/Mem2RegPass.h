#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class Mem2RegPass final : public ModulePass {
 public:
  std::string name() const override { return "mem2reg"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
