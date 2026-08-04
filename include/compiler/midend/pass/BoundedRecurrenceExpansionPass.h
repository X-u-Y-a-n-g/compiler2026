#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class BoundedRecurrenceExpansionPass final : public ModulePass {
 public:
  std::string name() const override { return "bounded-recurrence-expansion"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
