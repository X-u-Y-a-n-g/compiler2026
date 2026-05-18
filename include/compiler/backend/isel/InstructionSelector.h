#pragma once

#include "MachineModule.h"
#include "TargetInfo.h"
#include "../../ir/core/Module.h"

namespace compiler::backend::isel {

class InstructionSelector {
 public:
  compiler::backend::mir::MachineModule select(const compiler::ir::core::Module& module,
                                               const compiler::backend::target::TargetInfo& target) const;
};

}  // namespace compiler::backend::isel
