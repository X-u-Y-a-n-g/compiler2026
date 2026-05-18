#pragma once

#include <string>

#include "Module.h"

namespace compiler::ir::printer {

class IRPrinter {
 public:
  std::string print(const compiler::ir::core::Module& module) const;
};

}  // namespace compiler::ir::printer
