#pragma once

#include "../mir/MachineModule.h"

namespace compiler::backend::opt {

class AArch64PostRAVectorization {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
