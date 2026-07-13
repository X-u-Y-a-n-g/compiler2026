#include "../../../include/compiler/backend/opt/CompareBranchFusion.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

bool isVirtualRegister(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister;
}

bool isIntegerCompare(const MachineInstruction& instruction) {
  return instruction.hasResult && instruction.opcode.rfind("icmp.", 0) == 0 &&
         instruction.operands.size() == 2 && isVirtualRegister(instruction.result);
}

bool isConditionalBranch(const MachineInstruction& instruction) {
  return instruction.opcode == "br" && instruction.operands.size() == 3 &&
         isVirtualRegister(instruction.operands[0]);
}

}  // namespace

void CompareBranchFusion::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);
  for (auto& function : module.functions) {
    std::unordered_map<std::string, std::size_t> useCounts;
    std::vector<MachineOperand> uses;
    for (const auto& block : function.blocks) {
      for (const auto& instruction : block.instructions) {
        adapter.enumerateUses(instruction, uses);
        for (const auto& operand : uses) {
          if (isVirtualRegister(operand)) {
            ++useCounts[operand.text];
          }
        }
      }
    }

    for (auto& block : function.blocks) {
      std::vector<MachineInstruction> fused;
      fused.reserve(block.instructions.size());
      for (std::size_t index = 0; index < block.instructions.size();) {
        if (index + 1 < block.instructions.size()) {
          const auto& compare = block.instructions[index];
          const auto& branch = block.instructions[index + 1];
          if (isIntegerCompare(compare) && isConditionalBranch(branch) &&
              branch.operands[0].text == compare.result.text &&
              useCounts[compare.result.text] == 1) {
            std::vector<MachineOperand> operands;
            operands.reserve(4);
            operands.push_back(compare.operands[0]);
            operands.push_back(compare.operands[1]);
            operands.push_back(branch.operands[1]);
            operands.push_back(branch.operands[2]);
            fused.push_back(MachineInstruction::target(
                "br." + compare.opcode, std::move(operands)));
            index += 2;
            continue;
          }
        }
        fused.push_back(std::move(block.instructions[index]));
        ++index;
      }
      block.instructions = std::move(fused);
    }
  }
}

}  // namespace compiler::backend::opt
