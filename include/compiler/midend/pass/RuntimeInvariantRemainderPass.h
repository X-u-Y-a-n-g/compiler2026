#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RuntimeInvariantRemainderPass final : public ModulePass {
public:
  std::string name() const override { return "runtime-invariant-remainder"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
