#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class StoreProducerReductionFusionPass final : public ModulePass {
 public:
  std::string name() const override {
    return "store-producer-reduction-fusion";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
