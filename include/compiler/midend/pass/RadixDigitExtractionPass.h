#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class RadixDigitExtractionPass final : public ModulePass {
 public:
  std::string name() const override { return "radix-digit-extraction"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
