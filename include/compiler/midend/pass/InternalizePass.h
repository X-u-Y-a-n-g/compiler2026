#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class InternalizePass final : public ModulePass {
 public:
  std::string name() const override { return "internalize"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
