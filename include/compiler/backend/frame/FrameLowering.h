#pragma once

#include "MachineModule.h"

namespace compiler::backend::frame {

class FrameLowering {
 public:
  void lower(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::frame
