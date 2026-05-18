#include "../../../include/compiler/backend/opt/PhiElimination.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::common::CFGBuilder;
using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;

std::size_t insertionPoint(const MachineBlock& block) {
  std::size_t index = block.instructions.size();
  while (index > 0) {
    const auto& instruction = block.instructions[index - 1];
    if (instruction.opcode == "br" || instruction.opcode == "ret") {
      --index;
      continue;
    }
    break;
  }
  return index;
}

}  // namespace

void PhiElimination::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    std::unordered_map<std::string, std::size_t> blockIndexByName;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
      blockIndexByName.emplace(function.blocks[i].name, i);
    }

    const auto cfg = CFGBuilder{}.build(function, adapter);
    (void)cfg;

    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
      auto& block = function.blocks[blockIndex];
      std::vector<MachineInstruction> remaining;
      remaining.reserve(block.instructions.size());

      for (const auto& instruction : block.instructions) {
        if (instruction.kind != MachineInstructionKind::Phi) {
          remaining.push_back(instruction);
          continue;
        }

        for (const auto& incoming : instruction.phiInputs) {
          const auto predIt = blockIndexByName.find(incoming.block);
          if (predIt == blockIndexByName.end()) {
            continue;
          }

          auto& predecessor = function.blocks[predIt->second];
          const std::size_t insertAt = insertionPoint(predecessor);
          predecessor.instructions.insert(predecessor.instructions.begin() + insertAt,
                                          MachineInstruction::copy(instruction.result, incoming.value));
        }
      }

      block.instructions = std::move(remaining);
    }
  }
}

}  // namespace compiler::backend::opt
