#pragma once

#include <string>
#include <vector>

#include "compiler/ir/core/Module.h"

namespace compiler::ir::verify {

class IRVerifier {
 public:
  std::vector<std::string> verify(const compiler::ir::core::Module& module) const;
};

}  // namespace compiler::ir::verify
