#pragma once

#include <string>

#include "MachineModule.h"

namespace compiler::backend::asmcode {

class AArch64AsmPrinter {
 public:
  [[nodiscard]] std::string print(const compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::asmcode
