#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopInterchangeRowUpdatePass final : public ModulePass {
public:
  std::string name() const override { return "loop-interchange-row-update"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
