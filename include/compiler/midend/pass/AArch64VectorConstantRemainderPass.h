#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AArch64VectorConstantRemainderPass final : public ModulePass {
public:
  void run(compiler::ir::core::Module &module) override;
  [[nodiscard]] std::string name() const override {
    return "aarch64-vector-constant-remainder";
  }
};

} // namespace compiler::midend::pass
