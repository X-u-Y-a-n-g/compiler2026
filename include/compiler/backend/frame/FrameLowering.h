#pragma once

#include "compiler/backend/mir/MachineModule.h"

namespace compiler::backend::frame {

class FrameLowering {
 public:
  void lower(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::frame
