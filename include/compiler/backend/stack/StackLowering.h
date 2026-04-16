#pragma once

#include "compiler/backend/mir/MachineModule.h"

namespace compiler::backend::stack {

class StackLowering {
 public:
  void lower(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::stack
