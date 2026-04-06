#pragma once

#include <memory>
#include <vector>

#include "compiler/midend/pass/Pass.h"

namespace compiler::midend::pass {

class PassManager {
 public:
  void add(std::unique_ptr<ModulePass> pass);
  void run(compiler::ir::core::Module& module) const;

 private:
  std::vector<std::unique_ptr<ModulePass>> passes_;
};

}  // namespace compiler::midend::pass
