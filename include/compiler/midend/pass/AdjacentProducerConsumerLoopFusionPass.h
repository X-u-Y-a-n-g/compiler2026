#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AdjacentProducerConsumerLoopFusionPass final : public ModulePass {
 public:
  std::string name() const override {
    return "adjacent-producer-consumer-loop-fusion";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
