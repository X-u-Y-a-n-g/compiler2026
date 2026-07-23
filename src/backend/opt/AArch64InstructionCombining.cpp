#include "../../../include/compiler/backend/opt/AArch64InstructionCombining.h"

#include <cstdint>
#include <cstdlib>
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

bool sameVirtualRegister(const MachineOperand& lhs, const MachineOperand& rhs) {
  return isVirtualRegister(lhs) && isVirtualRegister(rhs) &&
         lhs.text == rhs.text && lhs.type.kind == rhs.type.kind;
}

bool isAddSubImmediateEncodable(std::int64_t value) {
  const std::uint64_t magnitude =
      value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                : static_cast<std::uint64_t>(value);
  return magnitude <= 4095U ||
         (magnitude % 4096U == 0 && magnitude / 4096U <= 4095U);
}

std::unordered_set<std::string> collectVirtualRegisterNames(
    const MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::unordered_set<std::string> names;
  std::vector<MachineOperand> uses;
  for (const auto& parameter : function.parameters) names.insert(parameter.name);
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instruction.hasResult && isVirtualRegister(instruction.result)) {
        names.insert(instruction.result.text);
      }
      adapter.enumerateUses(instruction, uses);
      for (const auto& operand : uses) {
        if (isVirtualRegister(operand)) names.insert(operand.text);
      }
    }
  }
  return names;
}

std::string freshVirtualRegisterName(std::unordered_set<std::string>& names,
                                     const std::string& prefix) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = prefix + "." + std::to_string(suffix);
    if (names.insert(candidate).second) return candidate;
  }
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
      if ((add.opcode != "add" && add.opcode != "sub") || !add.hasResult ||
          add.operands.size() != 2 ||
          (!add.result.type.isInt32() && !add.result.type.isInt64())) {
        continue;
      }
      for (std::size_t productIndex = 0; productIndex < 2; ++productIndex) {
        if (add.opcode == "sub" && productIndex == 0) continue;
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
            add.opcode == "sub" ? "msub" : "madd", add.result,
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

void combineCompareSelects(
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
    for (auto& select : instructions) {
      if (select.opcode != "select" || !select.hasResult ||
          select.operands.size() != 3 ||
          (!select.result.type.isInt32() && !select.result.type.isInt64() &&
           !select.result.type.isPointer()) ||
          !isVirtualRegister(select.operands[0]) ||
          useCounts[select.operands[0].text] != 1) {
        continue;
      }
      const auto found = definitions.find(select.operands[0].text);
      if (found == definitions.end() || found->second.block != block ||
          removals[block].find(found->second.instruction) !=
              removals[block].end()) {
        continue;
      }
      const auto& compare = instructions[found->second.instruction];
      if (!compare.hasResult || compare.opcode.rfind("icmp.", 0) != 0 ||
          compare.operands.size() != 2 ||
          (compare.opcode != "icmp.eq" && compare.opcode != "icmp.ne" &&
           compare.opcode != "icmp.slt" && compare.opcode != "icmp.sle" &&
           compare.opcode != "icmp.sgt" && compare.opcode != "icmp.sge")) {
        continue;
      }
      select = MachineInstruction::target(
          "select." + compare.opcode, select.result,
          {compare.operands[0], compare.operands[1], select.operands[1],
           select.operands[2]});
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

void shareBoundedModuloConstant(
    MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  auto names = collectVirtualRegisterNames(function, adapter);
  for (auto& block : function.blocks) {
    auto& instructions = block.instructions;
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(instructions.size());
    for (std::size_t index = 0; index < instructions.size();) {
      if (index + 1 < instructions.size()) {
        auto& subtract = instructions[index];
        auto& select = instructions[index + 1];
        const bool positiveCorrection =
            subtract.opcode == "sub" && subtract.hasResult &&
            subtract.operands.size() == 2 &&
            subtract.operands[1].kind == MachineOperandKind::ImmediateInt &&
            subtract.operands[1].intValue > 1 &&
            !isAddSubImmediateEncodable(subtract.operands[1].intValue) &&
            select.opcode == "select.icmp.sge" && select.hasResult &&
            select.operands.size() == 4 &&
            sameVirtualRegister(select.operands[0], subtract.operands[0]) &&
            select.operands[1].kind == MachineOperandKind::ImmediateInt &&
            select.operands[1].intValue == subtract.operands[1].intValue &&
            sameVirtualRegister(select.operands[2], subtract.result) &&
            sameVirtualRegister(select.operands[3], subtract.operands[0]);
        if (positiveCorrection) {
          const auto constant = MachineOperand::virtualRegister(
              freshVirtualRegisterName(names,
                                       "%aarch64.bounded.mod.constant"),
              subtract.operands[1].type);
          rewritten.push_back(
              MachineInstruction::copy(constant, subtract.operands[1]));
          subtract.operands[1] = constant;
          select.operands[1] = constant;
          rewritten.push_back(std::move(subtract));
          rewritten.push_back(std::move(select));
          index += 2;
          continue;
        }
      }
      rewritten.push_back(std::move(instructions[index++]));
    }
    instructions = std::move(rewritten);
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
    combineCompareSelects(function, adapter);
    if (std::getenv("COMPILER_DISABLE_BOUNDED_MODULO_REDUCTION") == nullptr) {
      shareBoundedModuloConstant(function, adapter);
    }
    foldIndexedAddresses(function, adapter);
  }
}

}  // namespace compiler::backend::opt
