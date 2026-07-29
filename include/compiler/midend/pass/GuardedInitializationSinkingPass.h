#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class GuardedInitializationSinkingPass final : public ModulePass {
public:
  std::string name() const override { return "guarded-initialization-sinking"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
