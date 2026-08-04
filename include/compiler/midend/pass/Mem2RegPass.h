#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class Mem2RegPass final : public ModulePass {
 public:
  explicit Mem2RegPass(bool canonicalizeNonEntryAllocas = false)
      : canonicalizeNonEntryAllocas_(canonicalizeNonEntryAllocas) {}

  std::string name() const override { return "mem2reg"; }
  void run(compiler::ir::core::Module& module) override;

 private:
  bool canonicalizeNonEntryAllocas_;
};

}  // namespace compiler::midend::pass
