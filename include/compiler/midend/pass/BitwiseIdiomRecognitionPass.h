#pragma once

#include "Pass.h"

namespace compiler::midend::pass {

class BitwiseIdiomRecognitionPass final : public ModulePass {
 public:
  std::string name() const override { return "bitwise-idiom-recognition"; }
  void run(compiler::ir::core::Module& module) override;
};

}  // namespace compiler::midend::pass
