#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class CompareSimplificationPass final : public ModulePass {
public:
  std::string name() const override { return "compare-simplification"; }
  bool run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
