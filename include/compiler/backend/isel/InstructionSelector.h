#pragma once

#include "compiler/backend/mir/MachineModule.h"
#include "compiler/backend/target/TargetInfo.h"
#include "compiler/ir/core/Module.h"

namespace compiler::backend::isel {

class InstructionSelector {
 public:
  compiler::backend::mir::MachineModule select(const compiler::ir::core::Module& module,
                                               const compiler::backend::target::TargetInfo& target) const;
};

}  // namespace compiler::backend::isel
