#pragma once

#include "compiler/backend/common/CFG.h"
#include "compiler/backend/mir/MachineModule.h"
#include "compiler/backend/target/TargetInstrAdapter.h"

namespace compiler::backend::common {

class CFGBuilder {
 public:
  ControlFlowGraph build(const compiler::backend::mir::MachineFunction& function,
                         const compiler::backend::target::TargetInstrAdapter& adapter) const;
};

}  // namespace compiler::backend::common
