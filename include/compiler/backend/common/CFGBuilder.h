#pragma once

#include "CFG.h"
#include "MachineModule.h"
#include "TargetInstrAdapter.h"

namespace compiler::backend::common {

class CFGBuilder {
 public:
  ControlFlowGraph build(const compiler::backend::mir::MachineFunction& function,
                         const compiler::backend::target::TargetInstrAdapter& adapter) const;
};

}  // namespace compiler::backend::common
