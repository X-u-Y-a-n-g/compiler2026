#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AArch64IntegerDotReductionPass final : public ModulePass {
public:
  std::string name() const override { return "aarch64-integer-dot-reduction"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
