#pragma once

#include "MachineModule.h"

namespace compiler::backend::opt {

class MachineDCE {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
