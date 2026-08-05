#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RecursiveCallSpecializationPass final : public ModulePass {
 public:
  std::string name() const override {
    return "recursive-call-specialization";
  }

  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
