#pragma once

#include "MachineModule.h"

namespace compiler::backend::opt {

class PhiElimination {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
