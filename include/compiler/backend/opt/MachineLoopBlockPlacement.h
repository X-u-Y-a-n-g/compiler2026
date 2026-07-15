#pragma once

#include "../mir/MachineModule.h"

namespace compiler::backend::opt {

// Places canonical natural loops as
//
//   body/latch -> header -> exit
//
// so the latch can fall through to the loop condition and the condition can
// branch backwards to the hot body while the exit remains the fallthrough.
// The transformation changes only physical block order; CFG edges and
// instructions are left unchanged.
class MachineLoopBlockPlacement {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
