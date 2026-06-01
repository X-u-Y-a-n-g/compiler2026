#pragma once

#include <memory>
#include <vector>

#include "Pass.h"

namespace compiler::midend::pass {

class PassManager {
 public:
  void add(std::unique_ptr<ModulePass> pass);
  [[nodiscard]] bool empty() const { return passes_.empty(); }
  bool run(compiler::ir::core::Module& module) const;
  bool runFixedPoint(compiler::ir::core::Module& module, int maxRounds) const;

 private:
  std::vector<std::unique_ptr<ModulePass>> passes_;
};

}  // namespace compiler::midend::pass
