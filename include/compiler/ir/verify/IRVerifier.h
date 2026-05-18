#pragma once

#include <string>
#include <vector>

#include "Module.h"

namespace compiler::ir::verify {

class IRVerifier {
 public:
  std::vector<std::string> verify(const compiler::ir::core::Module& module) const;
};

}  // namespace compiler::ir::verify
