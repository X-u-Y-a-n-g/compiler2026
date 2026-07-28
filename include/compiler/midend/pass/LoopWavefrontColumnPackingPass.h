#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class LoopWavefrontColumnPackingPass final : public ModulePass {
public:
  std::string name() const override { return "loop-wavefront-column-packing"; }
  void run(compiler::ir::core::Module &module) override;
};

} // namespace compiler::midend::pass
