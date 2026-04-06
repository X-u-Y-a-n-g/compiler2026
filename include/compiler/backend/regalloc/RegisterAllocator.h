#pragma once

#include "compiler/backend/mir/MachineModule.h"

namespace compiler::backend::regalloc {

class RegisterAllocator {
 public:
  void allocate(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::regalloc
