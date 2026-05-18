#pragma once

#include "MachineModule.h"

namespace compiler::backend::opt {

class StackObjectPromotion {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
