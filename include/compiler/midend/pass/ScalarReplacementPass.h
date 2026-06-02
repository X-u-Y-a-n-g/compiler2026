#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class ScalarReplacementPass final : public ModulePass {
 public:
  std::string name() const override { return "scalar-replacement"; }
  bool run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
