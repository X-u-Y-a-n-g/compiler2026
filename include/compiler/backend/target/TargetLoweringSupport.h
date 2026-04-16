#pragma once

#include "compiler/backend/mir/MachineModule.h"
#include "compiler/backend/target/TargetInfo.h"
#include "compiler/ir/core/Module.h"

namespace compiler::backend::target::detail {

compiler::backend::mir::MachineModule lowerModuleDefault(
    const compiler::ir::core::Module& module, const compiler::backend::target::TargetInfo& target);

}  // namespace compiler::backend::target::detail
