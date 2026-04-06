#include "compiler/backend/isel/InstructionSelector.h"

namespace compiler::backend::isel {

compiler::backend::mir::MachineModule InstructionSelector::select(
    const compiler::ir::core::Module& module,
    const compiler::backend::target::TargetInfo& target) const {
  compiler::backend::mir::MachineModule machineModule{target, {}};

  for (const auto& function : module.functions()) {
    compiler::backend::mir::MachineFunction machineFunction;
    machineFunction.name = function.name();

    for (const auto& block : function.blocks()) {
      compiler::backend::mir::MachineBlock machineBlock;
      machineBlock.name = block.name();

      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() == "ret") {
          if (!instruction.operands().empty()) {
            if (target.name() == "armv7") {
              machineBlock.instructions.push_back({"mov_ret", {instruction.operands().front()}});
            } else {
              machineBlock.instructions.push_back({"load_ret", {instruction.operands().front()}});
            }
          }
          machineBlock.instructions.push_back({"ret", {}});
        }
      }

      machineFunction.blocks.push_back(std::move(machineBlock));
    }

    machineModule.functions.push_back(std::move(machineFunction));
  }

  return machineModule;
}

}  // namespace compiler::backend::isel
