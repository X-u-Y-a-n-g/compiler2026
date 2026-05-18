#include "InstructionSelector.h"

#include "TargetBackend.h"

namespace compiler::backend::isel {

compiler::backend::mir::MachineModule InstructionSelector::select(
    const compiler::ir::core::Module& module,
    const compiler::backend::target::TargetInfo& target) const {
  return compiler::backend::target::loweringFor(target).lowerModule(module, target);
}

}  // namespace compiler::backend::isel
