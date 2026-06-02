#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class NameNormalizePass final : public ModulePass {
 public:
  std::string name() const override { return "name-normalize"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
