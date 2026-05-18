#pragma once

#include "MachineModule.h"

namespace compiler::backend::regalloc {

class RegisterAllocator {
 public:
  void allocate(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::regalloc
