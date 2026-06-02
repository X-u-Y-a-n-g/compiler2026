#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ReassociatePass final : public ModulePass {
public:
  std::string name() const override { return "reassociate"; }
  bool run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
