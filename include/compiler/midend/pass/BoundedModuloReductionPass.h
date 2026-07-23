#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class BoundedModuloReductionPass final : public ModulePass {
 public:
  std::string name() const override { return "bounded-modulo-reduction"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
