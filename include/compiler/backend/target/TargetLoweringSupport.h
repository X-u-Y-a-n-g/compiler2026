#pragma once

#include "MachineModule.h"
#include "TargetInfo.h"
#include "Module.h"

namespace compiler::backend::target::detail {

compiler::backend::mir::MachineModule lowerModuleDefault(
    const compiler::ir::core::Module& module, const compiler::backend::target::TargetInfo& target);

}  // namespace compiler::backend::target::detail
