#pragma once

#include <string>

#include "compiler/backend/mir/MachineModule.h"

namespace compiler::backend::asmcode {

class AsmPrinter {
 public:
  std::string print(const compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::asmcode
