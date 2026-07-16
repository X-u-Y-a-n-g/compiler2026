#pragma once

#include "../mir/MachineModule.h"

namespace compiler::backend::opt {

// Greedily lays out the whole machine CFG to turn explicit branch edges into
// fallthrough edges.  A move is retained only when it strictly reduces the
// number of control-transfer instructions required by the final layout.
class MachineTraceBlockPlacement {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
