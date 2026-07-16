#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RecursiveInliningPass final : public ModulePass {
public:
  std::string name() const override { return "recursive-inlining"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
