#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class OrderedRecurrenceUnrollPass final : public ModulePass {
 public:
  std::string name() const override { return "ordered-recurrence-unroll"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
