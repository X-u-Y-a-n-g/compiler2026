#pragma once

#include <string>

#include "Module.h"

namespace compiler::midend::pass {

class ModulePass {
 public:
  virtual ~ModulePass() = default;
  virtual std::string name() const = 0;
  virtual void run(compiler::ir::core::Module& module) = 0;
};

}  // namespace compiler::midend::pass
