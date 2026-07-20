#pragma once

#include "../mir/MachineModule.h"

namespace compiler::backend::opt {

class AArch64InstructionCombining {
 public:
  void lowerConstantDivisions(
      compiler::backend::mir::MachineModule& module) const;
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
