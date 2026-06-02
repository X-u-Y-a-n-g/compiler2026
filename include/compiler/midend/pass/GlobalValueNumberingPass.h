#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class GlobalValueNumberingPass final : public ModulePass {
public:
  std::string name() const override { return "global-value-numbering"; }
  bool run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
