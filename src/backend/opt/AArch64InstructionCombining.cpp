#include "../../../include/compiler/backend/opt/AArch64InstructionCombining.h"

#include <string>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {
namespace {

using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

struct Definition {
  std::size_t block = 0;
  std::size_t instruction = 0;
};

bool isVirtualRegister(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister;
}

bool isAddressCalculation(const MachineInstruction& instruction) {
  return (instruction.opcode == "gep" || instruction.opcode == "gep.linear" ||
          instruction.opcode == "getelementptr" ||
          instruction.opcode == "getelementptr.linear") &&
         instruction.hasResult && instruction.operands.size() == 3 &&
         instruction.operands[2].kind == MachineOperandKind::ImmediateInt;
}

std::size_t memoryAccessSize(const MachineInstruction& instruction) {
  const auto& type = instruction.opcode == "load"
                         ? instruction.result.type
                         : instruction.operands[0].type;
  return type.isInt64() || type.isPointer() ? 8U : 4U;
}

void combineMultiplyAdds(
    MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::unordered_map<std::string, Definition> definitions;
  std::unordered_map<std::string, std::size_t> useCounts;
  std::vector<MachineOperand> uses;
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    for (std::size_t instruction = 0;
         instruction < function.blocks[block].instructions.size();
         ++instruction) {
      const auto& current = function.blocks[block].instructions[instruction];
      if (current.hasResult && isVirtualRegister(current.result)) {
        definitions.emplace(current.result.text, Definition{block, instruction});
      }
      adapter.enumerateUses(current, uses);
      for (const auto& operand : uses) {
        if (isVirtualRegister(operand)) ++useCounts[operand.text];
      }
    }
  }

  std::vector<std::unordered_set<std::size_t>> removals(function.blocks.size());
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    auto& instructions = function.blocks[block].instructions;
    for (auto& add : instructions) {
      if (add.opcode != "add" || !add.hasResult ||
          add.operands.size() != 2 ||
          (!add.result.type.isInt32() && !add.result.type.isInt64())) {
        continue;
      }
      for (std::size_t productIndex = 0; productIndex < 2; ++productIndex) {
        const auto& product = add.operands[productIndex];
        if (!isVirtualRegister(product) || useCounts[product.text] != 1) continue;
        const auto definition = definitions.find(product.text);
        if (definition == definitions.end() || definition->second.block != block ||
            removals[block].find(definition->second.instruction) !=
                removals[block].end()) {
          continue;
        }
        const auto& multiply = instructions[definition->second.instruction];
        if (multiply.opcode != "mul" || !multiply.hasResult ||
            multiply.operands.size() != 2 ||
            multiply.result.type.kind != add.result.type.kind) {
          continue;
        }
        add = MachineInstruction::target(
            "madd", add.result,
            {multiply.operands[0], multiply.operands[1],
             add.operands[1 - productIndex]});
        removals[block].insert(definition->second.instruction);
        break;
      }
    }
  }

  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    if (removals[block].empty()) continue;
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(function.blocks[block].instructions.size() -
                       removals[block].size());
    for (std::size_t instruction = 0;
         instruction < function.blocks[block].instructions.size();
         ++instruction) {
      if (removals[block].find(instruction) == removals[block].end()) {
        rewritten.push_back(
            std::move(function.blocks[block].instructions[instruction]));
      }
    }
    function.blocks[block].instructions = std::move(rewritten);
  }
}

void foldIndexedAddresses(
    MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::unordered_map<std::string, Definition> definitions;
  std::unordered_map<std::string, std::size_t> useCounts;
  std::vector<MachineOperand> uses;
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    const auto& instructions = function.blocks[block].instructions;
    for (std::size_t instruction = 0; instruction < instructions.size();
         ++instruction) {
      const auto& current = instructions[instruction];
      if (current.hasResult && isVirtualRegister(current.result)) {
        definitions.emplace(current.result.text, Definition{block, instruction});
      }
      adapter.enumerateUses(current, uses);
      for (const auto& operand : uses) {
        if (isVirtualRegister(operand)) ++useCounts[operand.text];
      }
    }
  }

  std::vector<std::unordered_set<std::size_t>> removals(function.blocks.size());
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    auto& instructions = function.blocks[block].instructions;
    for (std::size_t instruction = 0; instruction < instructions.size();
         ++instruction) {
      auto& memory = instructions[instruction];
      const bool isLoad = memory.opcode == "load" && memory.hasResult &&
                          memory.operands.size() == 1;
      const bool isStore = memory.opcode == "store" &&
                           memory.operands.size() == 2;
      if (!isLoad && !isStore) continue;

      const std::size_t addressIndex = isLoad ? 0U : 1U;
      const auto address = memory.operands[addressIndex];
      if (!isVirtualRegister(address) || useCounts[address.text] != 1) continue;
      const auto found = definitions.find(address.text);
      if (found == definitions.end() || found->second.block != block ||
          found->second.instruction >= instruction) {
        continue;
      }
      const auto& gep = instructions[found->second.instruction];
      if (!isAddressCalculation(gep)) continue;

      const auto scale = gep.operands[2].intValue;
      const bool immediateIndex =
          gep.operands[1].kind == MachineOperandKind::ImmediateInt;
      if (scale <= 0 ||
          (!immediateIndex &&
           static_cast<std::uint64_t>(scale) != memoryAccessSize(memory))) {
        continue;
      }
      if (immediateIndex) {
        const __int128 offset =
            static_cast<__int128>(gep.operands[1].intValue) * scale;
        if (offset < std::numeric_limits<std::int64_t>::min() ||
            offset > std::numeric_limits<std::int64_t>::max()) {
          continue;
        }
      }

      if (isLoad) {
        memory = MachineInstruction::target(
            "load.indexed", memory.result,
            {gep.operands[0], gep.operands[1], gep.operands[2]});
      } else {
        memory = MachineInstruction::target(
            "store.indexed",
            {memory.operands[0], gep.operands[0], gep.operands[1],
             gep.operands[2]});
      }
      removals[block].insert(found->second.instruction);
    }
  }

  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    if (removals[block].empty()) continue;
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(function.blocks[block].instructions.size() -
                       removals[block].size());
    for (std::size_t instruction = 0;
         instruction < function.blocks[block].instructions.size();
         ++instruction) {
      if (removals[block].find(instruction) == removals[block].end()) {
        rewritten.push_back(
            std::move(function.blocks[block].instructions[instruction]));
      }
    }
    function.blocks[block].instructions = std::move(rewritten);
  }
}

}  // namespace

void AArch64InstructionCombining::run(
    compiler::backend::mir::MachineModule& module) const {
  if (module.target.name() != "aarch64") return;
  const auto& adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  for (auto& function : module.functions) {
    combineMultiplyAdds(function, adapter);
    foldIndexedAddresses(function, adapter);
  }
}

}  // namespace compiler::backend::opt
