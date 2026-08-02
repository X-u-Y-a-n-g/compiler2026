#include "../../../include/compiler/backend/opt/AArch64InstructionCombining.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <limits>
#include <optional>
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

bool samePhysicalRegister(const MachineOperand& lhs,
                          const MachineOperand& rhs) {
  return lhs.kind == MachineOperandKind::PhysicalRegister &&
         rhs.kind == MachineOperandKind::PhysicalRegister &&
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

struct PhysicalI32Load {
  MachineOperand destination;
  MachineOperand base;
  long long byteOffset = 0;
};

std::optional<PhysicalI32Load>
physicalI32Load(const MachineInstruction& instruction) {
  if (!instruction.hasResult || !instruction.result.type.isInt32() ||
      instruction.result.kind != MachineOperandKind::PhysicalRegister) {
    return std::nullopt;
  }
  if (instruction.opcode == "load" && instruction.operands.size() == 1 &&
      instruction.operands[0].kind == MachineOperandKind::PhysicalRegister) {
    return PhysicalI32Load{instruction.result, instruction.operands[0], 0};
  }
  if (instruction.opcode != "load.indexed" ||
      instruction.operands.size() != 3 ||
      instruction.operands[0].kind != MachineOperandKind::PhysicalRegister ||
      instruction.operands[1].kind != MachineOperandKind::ImmediateInt ||
      instruction.operands[2].kind != MachineOperandKind::ImmediateInt) {
    return std::nullopt;
  }
  const __int128 offset =
      static_cast<__int128>(instruction.operands[1].intValue) *
      instruction.operands[2].intValue;
  if (offset < std::numeric_limits<long long>::min() ||
      offset > std::numeric_limits<long long>::max()) {
    return std::nullopt;
  }
  return PhysicalI32Load{instruction.result, instruction.operands[0],
                         static_cast<long long>(offset)};
}

void combinePostRAPairedI32Loads(MachineFunction& function) {
  for (auto& block : function.blocks) {
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(block.instructions.size());
    for (std::size_t index = 0; index < block.instructions.size();) {
      if (index + 1 < block.instructions.size()) {
        const auto first = physicalI32Load(block.instructions[index]);
        const auto second = physicalI32Load(block.instructions[index + 1]);
        if (first.has_value() && second.has_value() &&
            samePhysicalRegister(first->base, second->base) &&
            !samePhysicalRegister(first->destination, second->destination)) {
          const PhysicalI32Load* low = &*first;
          const PhysicalI32Load* high = &*second;
          if (second->byteOffset < first->byteOffset) {
            low = &*second;
            high = &*first;
          }
          if (low->byteOffset + 4 == high->byteOffset &&
              low->byteOffset >= -256 && low->byteOffset <= 252 &&
              low->byteOffset % 4 == 0) {
            rewritten.push_back(MachineInstruction::target(
                "aarch64.ldp.i32",
                {low->destination, high->destination, low->base,
                 MachineOperand::immediateInt(
                     low->byteOffset,
                     compiler::backend::mir::ValueType::int32Type())}));
            index += 2;
            continue;
          }
        }
      }
      rewritten.push_back(std::move(block.instructions[index++]));
    }
    block.instructions = std::move(rewritten);
  }
}

struct StackZeroStore {
  MachineOperand base;
  long long byteOffset = 0;
};

std::optional<StackZeroStore> indexedStackZeroStore(
    const MachineInstruction& instruction) {
  if (instruction.opcode != "store.indexed" ||
      instruction.operands.size() != 4 ||
      instruction.operands[0].kind != MachineOperandKind::ImmediateInt ||
      instruction.operands[0].intValue != 0 ||
      !instruction.operands[0].type.isInt32() ||
      (instruction.operands[1].kind != MachineOperandKind::FrameIndex &&
       instruction.operands[1].kind != MachineOperandKind::StackObject) ||
      instruction.operands[2].kind != MachineOperandKind::ImmediateInt ||
      instruction.operands[3].kind != MachineOperandKind::ImmediateInt ||
      instruction.operands[3].intValue != 4) {
    return std::nullopt;
  }
  const __int128 offset =
      static_cast<__int128>(instruction.operands[2].intValue) * 4;
  if (offset < 0 || offset > std::numeric_limits<long long>::max()) {
    return std::nullopt;
  }
  return StackZeroStore{instruction.operands[1],
                        static_cast<long long>(offset)};
}

bool sameStackObject(const MachineOperand& lhs, const MachineOperand& rhs) {
  return lhs.kind == rhs.kind && lhs.index == rhs.index;
}

long long stackObjectFrameOffset(const MachineFunction& function,
                                 const MachineOperand& base) {
  const auto& object =
      function.stackObjects[static_cast<std::size_t>(base.index)];
  const long long cfaOffset = -static_cast<long long>(object.offset);
  return function.usesFramePointer
             ? cfaOffset
             : static_cast<long long>(function.frameSize) + cfaOffset;
}

std::size_t combinePostRAStackZeroRanges(MachineFunction& function) {
  constexpr std::size_t kStoresPerChunk = 8;
  constexpr std::size_t kBytesPerStore = 4;
  constexpr std::size_t kMaximumBytesPerPseudo = 256;

  std::size_t combinedStores = 0;
  for (auto& block : function.blocks) {
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(block.instructions.size());
    for (std::size_t index = 0; index < block.instructions.size();) {
      const auto first = indexedStackZeroStore(block.instructions[index]);
      if (!first.has_value()) {
        rewritten.push_back(std::move(block.instructions[index++]));
        continue;
      }

      std::size_t runEnd = index + 1;
      long long expectedOffset = first->byteOffset;
      while (runEnd < block.instructions.size()) {
        if (expectedOffset >
            std::numeric_limits<long long>::max() -
                static_cast<long long>(kBytesPerStore)) {
          break;
        }
        expectedOffset += kBytesPerStore;
        const auto next = indexedStackZeroStore(block.instructions[runEnd]);
        if (!next.has_value() ||
            !sameStackObject(first->base, next->base) ||
            next->byteOffset != expectedOffset) {
          break;
        }
        ++runEnd;
      }

      const std::size_t runStores = runEnd - index;
      std::size_t vectorStores =
          (runStores / kStoresPerChunk) * kStoresPerChunk;
      if (first->base.index < 0 ||
          static_cast<std::size_t>(first->base.index) >=
              function.stackObjects.size()) {
        vectorStores = 0;
      } else {
        const auto& object = function.stackObjects[static_cast<std::size_t>(
            first->base.index)];
        const auto byteCount = vectorStores * kBytesPerStore;
        if (first->byteOffset < 0 ||
            static_cast<unsigned long long>(first->byteOffset) + byteCount >
                object.size) {
          vectorStores = 0;
        } else {
          const long long frameOffset =
              stackObjectFrameOffset(function, first->base) +
              first->byteOffset;
          if (frameOffset % 16 != 0) vectorStores = 0;
        }
      }

      std::size_t emittedStores = 0;
      while (emittedStores < vectorStores) {
        const std::size_t remainingBytes =
            (vectorStores - emittedStores) * kBytesPerStore;
        const std::size_t byteCount =
            std::min(remainingBytes, kMaximumBytesPerPseudo);
        rewritten.push_back(MachineInstruction::target(
            "aarch64.zero.stack.range",
            {first->base,
             MachineOperand::immediateInt(
                 first->byteOffset +
                     static_cast<long long>(emittedStores * kBytesPerStore),
                 compiler::backend::mir::ValueType::int64Type()),
             MachineOperand::immediateInt(
                 static_cast<long long>(byteCount),
                 compiler::backend::mir::ValueType::int64Type())}));
        emittedStores += byteCount / kBytesPerStore;
      }
      combinedStores += vectorStores;

      for (std::size_t scalar = index + vectorStores; scalar < runEnd;
           ++scalar) {
        rewritten.push_back(std::move(block.instructions[scalar]));
      }
      index = runEnd;
    }
    block.instructions = std::move(rewritten);
  }
  return combinedStores;
}

bool isZeroImmediate(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::ImmediateInt &&
         operand.intValue == 0;
}

bool isFlagTransparentCountedLoopInstruction(
    const MachineInstruction& instruction) {
  return instruction.opcode == "add" || instruction.opcode == "copy" ||
         instruction.opcode == "gep" || instruction.opcode == "gep.linear" ||
         instruction.opcode == "getelementptr" ||
         instruction.opcode == "getelementptr.linear";
}

void combinePostRACountedBranches(MachineFunction& function) {
  for (auto& block : function.blocks) {
    auto& instructions = block.instructions;
    for (std::size_t subIndex = 0; subIndex < instructions.size(); ++subIndex) {
      auto& subtract = instructions[subIndex];
      if (subtract.opcode != "sub" || !subtract.hasResult ||
          subtract.result.kind != MachineOperandKind::PhysicalRegister ||
          (!subtract.result.type.isInt32() &&
           !subtract.result.type.isInt64()) ||
          subtract.operands.size() != 2 ||
          subtract.operands[1].kind != MachineOperandKind::ImmediateInt ||
          subtract.operands[1].intValue != 1) {
        continue;
      }

      std::optional<std::size_t> compareIndex;
      std::optional<std::size_t> branchIndex;
      MachineOperand predicate;
      for (std::size_t index = subIndex + 1;
           index < instructions.size() && index <= subIndex + 8; ++index) {
        const auto& current = instructions[index];
        if (!compareIndex.has_value()) {
          if (current.opcode == "icmp.ne" && current.hasResult &&
              current.result.kind == MachineOperandKind::PhysicalRegister &&
              current.operands.size() == 2 &&
              ((samePhysicalRegister(current.operands[0], subtract.result) &&
                isZeroImmediate(current.operands[1])) ||
               (samePhysicalRegister(current.operands[1], subtract.result) &&
                isZeroImmediate(current.operands[0])))) {
            compareIndex = index;
            predicate = current.result;
            continue;
          }
          if (!isFlagTransparentCountedLoopInstruction(current))
            break;
          continue;
        }
        if (current.opcode == "br" && current.operands.size() == 3 &&
            samePhysicalRegister(current.operands[0], predicate)) {
          branchIndex = index;
          break;
        }
        if (!isFlagTransparentCountedLoopInstruction(current))
          break;
      }
      if (!compareIndex.has_value() || !branchIndex.has_value())
        continue;

      subtract.opcode = "aarch64.subs";
      instructions[*branchIndex] = MachineInstruction::target(
          "aarch64.br.flags.ne",
          {instructions[*branchIndex].operands[1],
           instructions[*branchIndex].operands[2]});
      instructions.erase(instructions.begin() +
                         static_cast<std::ptrdiff_t>(*compareIndex));
      break;
    }
  }
}

void combinePostRAIndexedReloadInductions(MachineFunction& function) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_INDEX_RELOAD_RENAMING") !=
      nullptr) {
    return;
  }

  for (auto& block : function.blocks) {
    bool scratchIsFree = true;
    for (const auto& instruction : block.instructions) {
      if ((instruction.hasResult &&
           instruction.result.kind == MachineOperandKind::PhysicalRegister &&
           instruction.result.text == "x16") ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [](const MachineOperand& operand) {
                        return operand.kind ==
                                   MachineOperandKind::PhysicalRegister &&
                               operand.text == "x16";
                      })) {
        scratchIsFree = false;
        break;
      }
    }
    if (!scratchIsFree) continue;

    auto& instructions = block.instructions;
    for (std::size_t addIndex = 0; addIndex + 2 < instructions.size();
         ++addIndex) {
      auto& increment = instructions[addIndex];
      if (increment.opcode != "add" || !increment.hasResult ||
          increment.result.kind != MachineOperandKind::PhysicalRegister ||
          !increment.result.type.isInt32() ||
          increment.operands.size() != 2 ||
          increment.operands[0].kind !=
              MachineOperandKind::PhysicalRegister ||
          !increment.operands[0].type.isInt32() ||
          increment.operands[1].kind != MachineOperandKind::ImmediateInt ||
          increment.operands[1].intValue != 1 ||
          samePhysicalRegister(increment.result, increment.operands[0])) {
        continue;
      }

      auto& reload = instructions[addIndex + 1];
      if (reload.opcode != "load.indexed" || !reload.hasResult ||
          !samePhysicalRegister(reload.result, increment.operands[0]) ||
          reload.operands.size() != 3 ||
          reload.operands[0].kind != MachineOperandKind::PhysicalRegister ||
          samePhysicalRegister(reload.operands[0], increment.operands[0]) ||
          !samePhysicalRegister(reload.operands[1], increment.result) ||
          reload.operands[2].kind != MachineOperandKind::ImmediateInt ||
          reload.operands[2].intValue != 4) {
        continue;
      }

      std::optional<std::size_t> copyIndex;
      bool safe = true;
      const std::size_t searchEnd =
          std::min(instructions.size(), addIndex + 12);
      for (std::size_t index = addIndex + 2; index < searchEnd; ++index) {
        const auto& current = instructions[index];
        if (current.opcode == "copy" && current.hasResult &&
            samePhysicalRegister(current.result, increment.operands[0]) &&
            current.operands.size() == 1 &&
            samePhysicalRegister(current.operands[0], increment.result)) {
          copyIndex = index;
          break;
        }
        if ((current.hasResult &&
             (samePhysicalRegister(current.result, increment.operands[0]) ||
              samePhysicalRegister(current.result, increment.result))) ||
            std::any_of(current.operands.begin(), current.operands.end(),
                        [&](const MachineOperand& operand) {
                          return samePhysicalRegister(operand,
                                                      increment.result);
                        })) {
          safe = false;
          break;
        }
      }
      if (!safe || !copyIndex.has_value()) continue;

      const MachineOperand induction = increment.operands[0];
      const MachineOperand scratch = MachineOperand::physicalRegister(
          "x16", compiler::backend::mir::ValueType::int32Type());
      increment.result = induction;
      reload.result = scratch;
      reload.operands[1] = induction;
      for (std::size_t index = addIndex + 2; index < *copyIndex; ++index) {
        for (auto& operand : instructions[index].operands) {
          if (samePhysicalRegister(operand, induction)) operand = scratch;
        }
      }
      instructions.erase(instructions.begin() +
                         static_cast<std::ptrdiff_t>(*copyIndex));
      break;
    }
  }
}

void redirectPostRANoOpBlocks(MachineFunction& function) {
  std::unordered_map<std::string, std::string> redirects;
  for (std::size_t blockIndex = 1; blockIndex < function.blocks.size();
       ++blockIndex) {
    const auto& block = function.blocks[blockIndex];
    if (block.instructions.empty())
      continue;
    bool noOp = true;
    for (std::size_t index = 0; index + 1 < block.instructions.size();
         ++index) {
      const auto& instruction = block.instructions[index];
      if (instruction.opcode != "copy" || !instruction.hasResult ||
          instruction.operands.size() != 1 ||
          !samePhysicalRegister(instruction.result,
                                instruction.operands.front())) {
        noOp = false;
        break;
      }
    }
    const auto& terminator = block.instructions.back();
    if (noOp && terminator.opcode == "br" &&
        terminator.operands.size() == 1 &&
        terminator.operands.front().kind == MachineOperandKind::Label &&
        terminator.operands.front().text != block.name) {
      redirects.emplace(block.name, terminator.operands.front().text);
    }
  }
  if (redirects.empty())
    return;

  const auto resolve = [&](std::string name) {
    std::unordered_set<std::string> seen;
    while (seen.insert(name).second) {
      const auto found = redirects.find(name);
      if (found == redirects.end())
        break;
      name = found->second;
    }
    return name;
  };
  for (auto& block : function.blocks) {
    for (auto& instruction : block.instructions) {
      const bool branch = instruction.opcode == "br" ||
                          instruction.opcode.rfind("br.icmp.", 0) == 0 ||
                          instruction.opcode == "aarch64.br.flags.ne";
      if (!branch)
        continue;
      for (auto& operand : instruction.operands) {
        if (operand.kind == MachineOperandKind::Label)
          operand.text = resolve(operand.text);
      }
    }
  }
  function.blocks.erase(
      std::remove_if(function.blocks.begin() + 1, function.blocks.end(),
                     [&](const compiler::backend::mir::MachineBlock& block) {
                       return redirects.find(block.name) != redirects.end();
                     }),
      function.blocks.end());
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

void AArch64InstructionCombining::runPostRA(
    compiler::backend::mir::MachineModule& module) const {
  if (module.target.name() != "aarch64") return;
  std::size_t combinedZeroStores = 0;
  for (auto& function : module.functions) {
    combinePostRAPairedI32Loads(function);
    if (std::getenv("COMPILER_DISABLE_AARCH64_STACK_ZERO_RANGE_COMBINING") ==
        nullptr) {
      combinedZeroStores += combinePostRAStackZeroRanges(function);
    }
    combinePostRACountedBranches(function);
    combinePostRAIndexedReloadInductions(function);
    redirectPostRANoOpBlocks(function);
  }
  if (std::getenv("COMPILER_AARCH64_STACK_ZERO_RANGE_STATS") != nullptr) {
    std::cerr << "aarch64-stack-zero-range: combined-i32-stores="
              << combinedZeroStores << '\n';
  }
}

}  // namespace compiler::backend::opt
