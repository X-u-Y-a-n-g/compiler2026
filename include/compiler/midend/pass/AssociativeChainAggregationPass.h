#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class AssociativeChainAggregationPass final : public ModulePass {
 public:
  std::string name() const override {
    return "associative-chain-aggregation";
  }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
