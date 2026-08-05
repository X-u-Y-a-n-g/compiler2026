#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AArch64OrderedMemoryRecurrenceVectorizationPass final
    : public ModulePass {
public:
  [[nodiscard]] std::string name() const override {
    return "aarch64-ordered-memory-recurrence-vectorization";
  }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
