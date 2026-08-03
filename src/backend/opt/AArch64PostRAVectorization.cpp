#include "../../../include/compiler/backend/opt/AArch64PostRAVectorization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::backend::opt {
namespace {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::mir::ValueType;

bool isPhysical(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::PhysicalRegister;
}

bool sameRegister(const MachineOperand& lhs, const MachineOperand& rhs) {
  return isPhysical(lhs) && isPhysical(rhs) && lhs.text == rhs.text;
}

bool isNoOpCopy(const MachineInstruction& instruction) {
  return instruction.opcode == "copy" && instruction.hasResult &&
         instruction.operands.size() == 1 &&
         sameRegister(instruction.result, instruction.operands.front());
}

std::vector<std::size_t> meaningfulInstructions(const MachineBlock& block) {
  std::vector<std::size_t> result;
  for (std::size_t index = 0; index < block.instructions.size(); ++index) {
    if (!isNoOpCopy(block.instructions[index])) result.push_back(index);
  }
  return result;
}

std::optional<std::int64_t> constantStoredValue(
    const MachineBlock& block, std::size_t storeIndex,
    const MachineOperand& value) {
  if (value.kind == MachineOperandKind::ImmediateInt) return value.intValue;
  if (!isPhysical(value)) return std::nullopt;
  for (std::size_t index = storeIndex; index > 0; --index) {
    const auto& instruction = block.instructions[index - 1];
    if (!instruction.hasResult || !sameRegister(instruction.result, value)) {
      continue;
    }
    if (instruction.opcode == "copy" && instruction.operands.size() == 1 &&
        instruction.operands[0].kind == MachineOperandKind::ImmediateInt) {
      return instruction.operands[0].intValue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

bool usesReservedFillVector(const MachineFunction& function) {
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instruction.hasResult && isPhysical(instruction.result) &&
          (instruction.result.text == "v16" || instruction.result.text == "v17")) {
        return true;
      }
      for (const auto& operand : instruction.operands) {
        if (isPhysical(operand) &&
            (operand.text == "v16" || operand.text == "v17")) {
          return true;
        }
      }
    }
  }
  return false;
}

struct FillStore {
  MachineOperand base;
  std::int64_t value = 0;
};

bool isImmediate(const MachineOperand& operand, std::int64_t value) {
  return operand.kind == MachineOperandKind::ImmediateInt &&
         operand.intValue == value;
}

bool isLabel(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::Label;
}

bool isBranchTo(const MachineInstruction& instruction,
                const std::string& target) {
  return instruction.opcode == "br" && instruction.operands.size() == 1 &&
         isLabel(instruction.operands[0]) &&
         instruction.operands[0].text == target;
}

const MachineBlock* targetBlock(
    const MachineFunction& function,
    const std::unordered_map<std::string, std::size_t>& blockIndex,
    const MachineOperand& label) {
  if (!isLabel(label)) return nullptr;
  const auto found = blockIndex.find(label.text);
  return found == blockIndex.end() ? nullptr : &function.blocks[found->second];
}

bool usesVectorRange(const MachineFunction& function, unsigned first,
                     unsigned last) {
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      const auto overlaps = [&](const MachineOperand& operand) {
        if (!isPhysical(operand) || operand.text.size() < 2 ||
            (operand.text[0] != 'v' && operand.text[0] != 'q' &&
             operand.text[0] != 'd' && operand.text[0] != 's')) {
          return false;
        }
        char* end = nullptr;
        const auto number = std::strtoul(operand.text.c_str() + 1, &end, 10);
        return end != operand.text.c_str() + 1 && *end == '\0' &&
               number >= first && number <= last;
      };
      if ((instruction.hasResult && overlaps(instruction.result)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      overlaps)) {
        return true;
      }
    }
  }
  return false;
}

struct DominanceInfo {
  std::vector<std::vector<bool>> dominates;
  std::vector<unsigned> depth;
};

DominanceInfo computeDominance(
    const MachineFunction& function,
    const std::unordered_map<std::string, std::size_t>& blockIndex) {
  const std::size_t count = function.blocks.size();
  std::vector<std::vector<std::size_t>> predecessors(count);
  for (std::size_t source = 0; source < count; ++source) {
    for (const auto& instruction : function.blocks[source].instructions) {
      if (instruction.opcode != "br" &&
          instruction.opcode.rfind("br.icmp.", 0) != 0 &&
          instruction.opcode.rfind("br.fcmp.", 0) != 0) {
        continue;
      }
      for (const auto& operand : instruction.operands) {
        if (!isLabel(operand)) continue;
        const auto found = blockIndex.find(operand.text);
        if (found != blockIndex.end()) {
          predecessors[found->second].push_back(source);
        }
      }
    }
  }
  std::vector<std::vector<bool>> dom(count, std::vector<bool>(count, true));
  if (count != 0) {
    std::fill(dom[0].begin(), dom[0].end(), false);
    dom[0][0] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      std::vector<bool> next(count, true);
      if (predecessors[block].empty()) {
        std::fill(next.begin(), next.end(), false);
      } else {
        for (const auto predecessor : predecessors[block]) {
          for (std::size_t candidate = 0; candidate < count; ++candidate) {
            next[candidate] = next[candidate] && dom[predecessor][candidate];
          }
        }
      }
      next[block] = true;
      if (next != dom[block]) {
        dom[block] = std::move(next);
        changed = true;
      }
    }
  }
  std::vector<unsigned> depth(count, 0);
  for (std::size_t block = 0; block < count; ++block) {
    depth[block] = static_cast<unsigned>(
        std::count(dom[block].begin(), dom[block].end(), true));
  }
  return DominanceInfo{std::move(dom), std::move(depth)};
}

std::optional<std::string> dominatingGlobalSource(
    const MachineFunction& function, const DominanceInfo& dominance,
    std::size_t useBlock, const MachineOperand& registerOperand) {
  if (!isPhysical(registerOperand)) return std::nullopt;
  const MachineInstruction* best = nullptr;
  unsigned bestDepth = 0;
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    if (!dominance.dominates[useBlock][block]) continue;
    for (const auto& instruction : function.blocks[block].instructions) {
      if (!instruction.hasResult ||
          !sameRegister(instruction.result, registerOperand)) {
        continue;
      }
      if (dominance.depth[block] >= bestDepth) {
        best = &instruction;
        bestDepth = dominance.depth[block];
      }
    }
  }
  if (best == nullptr || best->opcode != "copy" ||
      best->operands.size() != 1 ||
      best->operands[0].kind != MachineOperandKind::GlobalAddress) {
    return std::nullopt;
  }
  return best->operands[0].text;
}

std::size_t migrateConstantFills(MachineFunction& function,
                                 std::size_t nextSite) {
  if (std::getenv("COMPILER_AARCH64_DISABLE_VECTOR_FILL") != nullptr ||
      usesReservedFillVector(function)) {
    return nextSite;
  }

  std::unordered_map<std::string, std::size_t> blockIndex;
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    blockIndex.emplace(function.blocks[index].name, index);
  }

  for (std::size_t bodyIndex = 0; bodyIndex < function.blocks.size();
       ++bodyIndex) {
    auto& body = function.blocks[bodyIndex];
    const auto bodyInstructions = meaningfulInstructions(body);
    if (bodyInstructions.size() < 3) continue;

    const auto& increment = body.instructions[bodyInstructions[bodyInstructions.size() - 2]];
    const auto& backedge = body.instructions[bodyInstructions.back()];
    if (increment.opcode != "add" || !increment.hasResult ||
        increment.operands.size() != 2 ||
        !sameRegister(increment.result, increment.operands[0]) ||
        increment.operands[1].kind != MachineOperandKind::ImmediateInt ||
        increment.operands[1].intValue != 1 || backedge.opcode != "br" ||
        backedge.operands.size() != 1 ||
        backedge.operands[0].kind != MachineOperandKind::Label) {
      continue;
    }
    const MachineOperand index = increment.result;
    if (index.text == "x16" || index.text == "x17" || index.text == "x18") {
      continue;
    }

    std::vector<FillStore> stores;
    bool valid = true;
    for (std::size_t position = 0; position + 2 < bodyInstructions.size();
         ++position) {
      const auto instructionIndex = bodyInstructions[position];
      const auto& instruction = body.instructions[instructionIndex];
      if (instruction.opcode != "store.indexed" ||
          instruction.operands.size() != 4 ||
          instruction.operands[3].kind != MachineOperandKind::ImmediateInt ||
          instruction.operands[3].intValue != 4 ||
          !sameRegister(instruction.operands[2], index) ||
          !isPhysical(instruction.operands[1])) {
        valid = false;
        break;
      }
      const auto value =
          constantStoredValue(body, instructionIndex, instruction.operands[0]);
      if (!value.has_value() ||
          *value < std::numeric_limits<std::int32_t>::min() ||
          *value > std::numeric_limits<std::int32_t>::max()) {
        valid = false;
        break;
      }
      if (instruction.operands[1].text == "x16" ||
          instruction.operands[1].text == "x17" ||
          instruction.operands[1].text == "x18" ||
          std::any_of(stores.begin(), stores.end(), [&](const FillStore& store) {
            return store.base.text == instruction.operands[1].text;
          })) {
        valid = false;
        break;
      }
      stores.push_back(FillStore{instruction.operands[1], *value});
    }
    if (!valid || stores.empty() || stores.size() > 2) continue;

    const auto headerFound = blockIndex.find(backedge.operands[0].text);
    if (headerFound == blockIndex.end()) continue;
    const std::size_t headerIndex = headerFound->second;
    const auto headerInstructions =
        meaningfulInstructions(function.blocks[headerIndex]);
    if (headerInstructions.size() != 1) continue;
    const auto& condition =
        function.blocks[headerIndex].instructions[headerInstructions.front()];
    if (condition.opcode != "br.icmp.slt" || condition.operands.size() != 4 ||
        !sameRegister(condition.operands[0], index) ||
        condition.operands[2].kind != MachineOperandKind::Label ||
        condition.operands[2].text != body.name ||
        condition.operands[3].kind != MachineOperandKind::Label) {
      continue;
    }
    const MachineOperand bound = condition.operands[1];
    if ((!isPhysical(bound) &&
         bound.kind != MachineOperandKind::ImmediateInt) ||
        (isPhysical(bound) &&
         (bound.text == "x16" || bound.text == "x17" ||
          bound.text == "x18"))) {
      continue;
    }

    std::optional<std::pair<std::size_t, std::size_t>> preheader;
    for (std::size_t candidate = 0; candidate < function.blocks.size();
         ++candidate) {
      if (candidate == bodyIndex) continue;
      auto& block = function.blocks[candidate];
      const auto instructions = meaningfulInstructions(block);
      if (instructions.empty()) continue;
      const auto branchIndex = instructions.back();
      const auto& branch = block.instructions[branchIndex];
      if (branch.opcode != "br" || branch.operands.size() != 1 ||
          branch.operands[0].kind != MachineOperandKind::Label ||
          branch.operands[0].text != function.blocks[headerIndex].name) {
        continue;
      }
      bool initialized = false;
      for (std::size_t position = 0; position + 1 < instructions.size();
           ++position) {
        const auto& instruction = block.instructions[instructions[position]];
        if (instruction.opcode == "copy" && instruction.hasResult &&
            sameRegister(instruction.result, index) &&
            instruction.operands.size() == 1 &&
            instruction.operands[0].kind == MachineOperandKind::ImmediateInt &&
            instruction.operands[0].intValue == 0) {
          initialized = true;
        }
      }
      if (!initialized || preheader.has_value()) {
        preheader.reset();
        initialized = false;
        break;
      }
      preheader = std::pair{candidate, branchIndex};
    }
    if (!preheader.has_value()) continue;

    std::vector<MachineOperand> operands;
    operands.reserve(6 + stores.size() * 2);
    operands.push_back(index);
    operands.push_back(bound);
    operands.push_back(MachineOperand::label(function.blocks[headerIndex].name));
    operands.push_back(MachineOperand::immediateInt(
        static_cast<std::int64_t>(nextSite), ValueType::int64Type()));
    operands.push_back(MachineOperand::immediateInt(
        std::getenv("COMPILER_AARCH64_DISABLE_VECTOR_FILL_UNROLL") == nullptr
            ? 16
            : 4));
    operands.push_back(MachineOperand::immediateInt(
        static_cast<std::int64_t>(stores.size())));
    for (const auto& store : stores) {
      operands.push_back(store.base);
      operands.push_back(MachineOperand::immediateInt(store.value));
    }
    function.blocks[preheader->first].instructions[preheader->second] =
        MachineInstruction::target("aarch64.neon.vector.fill",
                                   std::move(operands));
    ++nextSite;
  }
  return nextSite;
}

std::size_t migrateConditionalMatrixReductions(MachineFunction& function,
                                               std::size_t nextSite) {
  if (std::getenv(
          "COMPILER_AARCH64_DISABLE_CONDITIONAL_MATRIX_REDUCTION") != nullptr ||
      usesVectorRange(function, 20, 27)) {
    return nextSite;
  }

  std::unordered_map<std::string, std::size_t> blockIndex;
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    blockIndex.emplace(function.blocks[index].name, index);
  }
  const auto dominance = computeDominance(function, blockIndex);
  const auto globalSource = [&](std::size_t useBlock,
                                const MachineOperand& operand)
      -> std::optional<std::string> {
    if (operand.kind == MachineOperandKind::GlobalAddress) return operand.text;
    return dominatingGlobalSource(function, dominance, useBlock, operand);
  };

  for (std::size_t rowIndex = 0; rowIndex < function.blocks.size(); ++rowIndex) {
    auto& row = function.blocks[rowIndex];
    const auto rowPositions = meaningfulInstructions(row);
    if (rowPositions.size() != 5) continue;
    std::vector<const MachineInstruction*> rowAddresses;
    const MachineInstruction* initializeJ = nullptr;
    std::size_t entryBranchIndex = 0;
    const MachineInstruction* entryBranch = nullptr;
    bool rowValid = true;
    for (const auto position : rowPositions) {
      const auto& instruction = row.instructions[position];
      if (instruction.opcode == "getelementptr" && instruction.hasResult &&
          instruction.operands.size() == 3 &&
          isPhysical(instruction.result) &&
          isPhysical(instruction.operands[1]) &&
          instruction.operands[2].kind == MachineOperandKind::ImmediateInt) {
        rowAddresses.push_back(&instruction);
      } else if (instruction.opcode == "copy" && instruction.hasResult &&
                 instruction.operands.size() == 1 &&
                 isPhysical(instruction.result) &&
                 isImmediate(instruction.operands[0], 0)) {
        if (initializeJ != nullptr) rowValid = false;
        initializeJ = &instruction;
      } else if (instruction.opcode == "br" &&
                 instruction.operands.size() == 1 &&
                 isLabel(instruction.operands[0])) {
        if (entryBranch != nullptr) rowValid = false;
        entryBranch = &instruction;
        entryBranchIndex = position;
      } else {
        rowValid = false;
      }
    }
    if (!rowValid || rowAddresses.size() != 3 || initializeJ == nullptr ||
        entryBranch == nullptr) {
      continue;
    }
    const MachineOperand j = initializeJ->result;
    const MachineBlock* middleHeader =
        targetBlock(function, blockIndex, entryBranch->operands[0]);
    if (middleHeader == nullptr) continue;
    const auto middleHeaderPositions = meaningfulInstructions(*middleHeader);
    if (middleHeaderPositions.size() != 1) continue;
    const auto& middleCondition =
        middleHeader->instructions[middleHeaderPositions[0]];
    if (middleCondition.opcode != "br.icmp.slt" ||
        middleCondition.operands.size() != 4 ||
        !sameRegister(middleCondition.operands[0], j) ||
        middleCondition.operands[1].kind != MachineOperandKind::ImmediateInt ||
        !isLabel(middleCondition.operands[2]) ||
        !isLabel(middleCondition.operands[3])) {
      continue;
    }
    const std::int64_t extent = middleCondition.operands[1].intValue;
    if (extent < 4 || extent % 4 != 0 || extent > 0x1fffffff) continue;
    const MachineBlock* middleBody =
        targetBlock(function, blockIndex, middleCondition.operands[2]);
    const MachineBlock* middleEnd =
        targetBlock(function, blockIndex, middleCondition.operands[3]);
    if (middleBody == nullptr || middleEnd == nullptr) continue;

    const auto middleBodyPositions = meaningfulInstructions(*middleBody);
    if (middleBodyPositions.size() != 6 && middleBodyPositions.size() != 7) {
      continue;
    }
    const auto& initializeAccumulator =
        middleBody->instructions[middleBodyPositions[0]];
    const auto& firstColumn =
        middleBody->instructions[middleBodyPositions[1]];
    const auto& secondColumn =
        middleBody->instructions[middleBodyPositions[2]];
    const bool hasPredicateAddressInduction = middleBodyPositions.size() == 7;
    const MachineInstruction* initializePredicateAddress =
        hasPredicateAddressInduction
            ? &middleBody->instructions[middleBodyPositions[3]]
            : nullptr;
    const std::size_t middleSuffix = hasPredicateAddressInduction ? 4 : 3;
    const auto& initializeBound =
        middleBody->instructions[middleBodyPositions[middleSuffix]];
    const auto& initializeK =
        middleBody->instructions[middleBodyPositions[middleSuffix + 1]];
    const auto& enterInner =
        middleBody->instructions[middleBodyPositions[middleSuffix + 2]];
    if (initializeAccumulator.opcode != "copy" ||
        !initializeAccumulator.hasResult ||
        initializeAccumulator.operands.size() != 1 ||
        !isImmediate(initializeAccumulator.operands[0], 0) ||
        firstColumn.opcode != "gep" || !firstColumn.hasResult ||
        firstColumn.operands.size() != 3 ||
        !sameRegister(firstColumn.operands[1], j) ||
        !isImmediate(firstColumn.operands[2], 4) ||
        secondColumn.opcode != "gep" || !secondColumn.hasResult ||
        secondColumn.operands.size() != 3 ||
        !sameRegister(secondColumn.operands[1], j) ||
        !isImmediate(secondColumn.operands[2], 4) ||
        (initializePredicateAddress != nullptr &&
         (initializePredicateAddress->opcode != "getelementptr" ||
          !initializePredicateAddress->hasResult ||
          initializePredicateAddress->operands.size() != 3 ||
          !sameRegister(initializePredicateAddress->operands[0],
                        secondColumn.result) ||
          !isImmediate(initializePredicateAddress->operands[1], 0) ||
          !isImmediate(initializePredicateAddress->operands[2], extent * 4))) ||
        initializeBound.opcode != "copy" || !initializeBound.hasResult ||
        initializeBound.operands.size() != 1 ||
        !isImmediate(initializeBound.operands[0], extent) ||
        initializeK.opcode != "copy" || !initializeK.hasResult ||
        initializeK.operands.size() != 1 ||
        !isImmediate(initializeK.operands[0], 0) ||
        enterInner.opcode != "br" || enterInner.operands.size() != 1 ||
        !isLabel(enterInner.operands[0])) {
      continue;
    }
    const MachineOperand accumulator = initializeAccumulator.result;
    const MachineOperand k = initializeK.result;
    const MachineOperand bound = initializeBound.result;
    const MachineBlock* innerHeader =
        targetBlock(function, blockIndex, enterInner.operands[0]);
    if (innerHeader == nullptr) continue;
    const auto innerHeaderPositions = meaningfulInstructions(*innerHeader);
    if (innerHeaderPositions.size() != 1) continue;
    const auto& innerCondition =
        innerHeader->instructions[innerHeaderPositions[0]];
    if (innerCondition.opcode != "br.icmp.slt" ||
        innerCondition.operands.size() != 4 ||
        !sameRegister(innerCondition.operands[0], k) ||
        !sameRegister(innerCondition.operands[1], bound) ||
        !isLabel(innerCondition.operands[2]) ||
        !isLabel(innerCondition.operands[3])) {
      continue;
    }
    const MachineBlock* innerBody =
        targetBlock(function, blockIndex, innerCondition.operands[2]);
    const MachineBlock* innerEnd =
        targetBlock(function, blockIndex, innerCondition.operands[3]);
    if (innerBody == nullptr || innerEnd == nullptr) continue;

    const auto innerBodyPositions = meaningfulInstructions(*innerBody);
    const std::size_t expectedInnerBodySize =
        hasPredicateAddressInduction ? 5 : 6;
    if (innerBodyPositions.size() != expectedInnerBodySize) continue;
    const auto& predicateRowLoad =
        innerBody->instructions[innerBodyPositions[0]];
    const MachineInstruction* predicateAddress =
        hasPredicateAddressInduction
            ? nullptr
            : &innerBody->instructions[innerBodyPositions[1]];
    const std::size_t predicateColumnPosition =
        hasPredicateAddressInduction ? 1 : 2;
    const auto& predicateColumnLoad =
        innerBody->instructions[innerBodyPositions[predicateColumnPosition]];
    const auto& predicateAnd =
        innerBody->instructions[innerBodyPositions[predicateColumnPosition + 1]];
    const auto& predicateLowBit =
        innerBody->instructions[innerBodyPositions[predicateColumnPosition + 2]];
    const auto& predicateBranch =
        innerBody->instructions[innerBodyPositions[predicateColumnPosition + 3]];
    const std::int64_t stride = extent * 4;
    if (predicateRowLoad.opcode != "load.indexed" ||
        !predicateRowLoad.hasResult || predicateRowLoad.operands.size() != 3 ||
        !sameRegister(predicateRowLoad.operands[1], k) ||
        !isImmediate(predicateRowLoad.operands[2], 4) ||
        (predicateAddress != nullptr &&
         (predicateAddress->opcode != "getelementptr" ||
          !predicateAddress->hasResult ||
          predicateAddress->operands.size() != 3 ||
          !sameRegister(predicateAddress->operands[0], secondColumn.result) ||
          !sameRegister(predicateAddress->operands[1], k) ||
          !isImmediate(predicateAddress->operands[2], stride))) ||
        predicateColumnLoad.opcode != "load" ||
        !predicateColumnLoad.hasResult ||
        predicateColumnLoad.operands.size() != 1 ||
        !(predicateAddress != nullptr
              ? sameRegister(predicateColumnLoad.operands[0],
                             predicateAddress->result)
              : sameRegister(predicateColumnLoad.operands[0],
                             initializePredicateAddress->result)) ||
        predicateAnd.opcode != "and" || !predicateAnd.hasResult ||
        predicateAnd.operands.size() != 2 ||
        !sameRegister(predicateAnd.operands[0], predicateRowLoad.result) ||
        !sameRegister(predicateAnd.operands[1], predicateColumnLoad.result) ||
        predicateLowBit.opcode != "and" || !predicateLowBit.hasResult ||
        predicateLowBit.operands.size() != 2 ||
        !sameRegister(predicateLowBit.operands[0], predicateAnd.result) ||
        !isImmediate(predicateLowBit.operands[1], 1) ||
        predicateBranch.opcode != "br.icmp.eq" ||
        predicateBranch.operands.size() != 4 ||
        !sameRegister(predicateBranch.operands[0], predicateLowBit.result) ||
        !isImmediate(predicateBranch.operands[1], 0) ||
        !isLabel(predicateBranch.operands[2]) ||
        !isLabel(predicateBranch.operands[3])) {
      continue;
    }
    const MachineBlock* thenBlock =
        targetBlock(function, blockIndex, predicateBranch.operands[2]);
    const MachineBlock* ifEnd =
        targetBlock(function, blockIndex, predicateBranch.operands[3]);
    if (thenBlock == nullptr || ifEnd == nullptr) continue;

    const auto thenPositions = meaningfulInstructions(*thenBlock);
    if (thenPositions.size() != 5) continue;
    const auto& productRowLoad = thenBlock->instructions[thenPositions[0]];
    const auto& productAddress = thenBlock->instructions[thenPositions[1]];
    const auto& productColumnLoad = thenBlock->instructions[thenPositions[2]];
    const auto& multiplyAdd = thenBlock->instructions[thenPositions[3]];
    const auto& leaveThen = thenBlock->instructions[thenPositions[4]];
    if (productRowLoad.opcode != "load.indexed" ||
        !productRowLoad.hasResult || productRowLoad.operands.size() != 3 ||
        !sameRegister(productRowLoad.operands[1], k) ||
        !isImmediate(productRowLoad.operands[2], 4) ||
        productAddress.opcode != "getelementptr" ||
        !productAddress.hasResult || productAddress.operands.size() != 3 ||
        !sameRegister(productAddress.operands[0], firstColumn.result) ||
        !sameRegister(productAddress.operands[1], k) ||
        !isImmediate(productAddress.operands[2], stride) ||
        productColumnLoad.opcode != "load" ||
        !productColumnLoad.hasResult ||
        productColumnLoad.operands.size() != 1 ||
        !sameRegister(productColumnLoad.operands[0], productAddress.result) ||
        multiplyAdd.opcode != "madd" || !multiplyAdd.hasResult ||
        multiplyAdd.operands.size() != 3 ||
        !sameRegister(multiplyAdd.result, accumulator) ||
        !sameRegister(multiplyAdd.operands[2], accumulator) ||
        !((sameRegister(multiplyAdd.operands[0], productRowLoad.result) &&
           sameRegister(multiplyAdd.operands[1], productColumnLoad.result)) ||
          (sameRegister(multiplyAdd.operands[1], productRowLoad.result) &&
           sameRegister(multiplyAdd.operands[0], productColumnLoad.result))) ||
        !isBranchTo(leaveThen, ifEnd->name)) {
      continue;
    }

    const auto ifEndPositions = meaningfulInstructions(*ifEnd);
    const std::size_t expectedIfEndSize =
        hasPredicateAddressInduction ? 3 : 2;
    if (ifEndPositions.size() != expectedIfEndSize) continue;
    const auto& incrementK = ifEnd->instructions[ifEndPositions[0]];
    const MachineInstruction* incrementPredicateAddress =
        hasPredicateAddressInduction
            ? &ifEnd->instructions[ifEndPositions[1]]
            : nullptr;
    const auto& innerBackedge =
        ifEnd->instructions[ifEndPositions[expectedIfEndSize - 1]];
    if (incrementK.opcode != "add" || !incrementK.hasResult ||
        incrementK.operands.size() != 2 ||
        !sameRegister(incrementK.result, k) ||
        !sameRegister(incrementK.operands[0], k) ||
        !isImmediate(incrementK.operands[1], 1) ||
        (incrementPredicateAddress != nullptr &&
         (incrementPredicateAddress->opcode != "getelementptr" ||
          !incrementPredicateAddress->hasResult ||
          incrementPredicateAddress->operands.size() != 3 ||
          !sameRegister(incrementPredicateAddress->result,
                        initializePredicateAddress->result) ||
          !sameRegister(incrementPredicateAddress->operands[0],
                        initializePredicateAddress->result) ||
          !isImmediate(incrementPredicateAddress->operands[1], 1) ||
          !isImmediate(incrementPredicateAddress->operands[2], stride))) ||
        !isBranchTo(innerBackedge, innerHeader->name)) {
      continue;
    }

    const auto innerEndPositions = meaningfulInstructions(*innerEnd);
    if (innerEndPositions.size() != 3) continue;
    const auto& resultStore = innerEnd->instructions[innerEndPositions[0]];
    const auto& incrementJ = innerEnd->instructions[innerEndPositions[1]];
    const auto& middleBackedge = innerEnd->instructions[innerEndPositions[2]];
    if (resultStore.opcode != "store.indexed" ||
        resultStore.operands.size() != 4 ||
        !sameRegister(resultStore.operands[0], accumulator) ||
        !sameRegister(resultStore.operands[2], j) ||
        !isImmediate(resultStore.operands[3], 4) ||
        incrementJ.opcode != "add" || !incrementJ.hasResult ||
        incrementJ.operands.size() != 2 ||
        !sameRegister(incrementJ.result, j) ||
        !sameRegister(incrementJ.operands[0], j) ||
        !isImmediate(incrementJ.operands[1], 1) ||
        !isBranchTo(middleBackedge, middleHeader->name)) {
      continue;
    }

    const MachineInstruction* firstRowAddress = nullptr;
    const MachineInstruction* secondRowAddress = nullptr;
    const MachineInstruction* outputRowAddress = nullptr;
    for (const auto* address : rowAddresses) {
      if (sameRegister(address->operands[0], firstColumn.operands[0])) {
        firstRowAddress = address;
      }
      if (sameRegister(address->operands[0], secondColumn.operands[0])) {
        secondRowAddress = address;
      }
      if (sameRegister(address->result, resultStore.operands[1])) {
        outputRowAddress = address;
      }
    }
    if (firstRowAddress == nullptr || secondRowAddress == nullptr ||
        outputRowAddress == nullptr || firstRowAddress == secondRowAddress ||
        outputRowAddress == firstRowAddress ||
        outputRowAddress == secondRowAddress ||
        !sameRegister(predicateRowLoad.operands[0], firstRowAddress->result) ||
        !sameRegister(productRowLoad.operands[0], secondRowAddress->result)) {
      continue;
    }
    const auto firstGlobal = globalSource(rowIndex, firstRowAddress->operands[0]);
    const auto secondGlobal =
        globalSource(rowIndex, secondRowAddress->operands[0]);
    const auto outputGlobal =
        globalSource(rowIndex, outputRowAddress->operands[0]);
    if (!firstGlobal.has_value() || !secondGlobal.has_value() ||
        !outputGlobal.has_value() || *firstGlobal == *secondGlobal ||
        *firstGlobal == *outputGlobal || *secondGlobal == *outputGlobal) {
      continue;
    }
    const auto sameOuterAndStride = [&](const MachineInstruction* address) {
      return sameRegister(address->operands[1],
                          firstRowAddress->operands[1]) &&
             address->operands[2].intValue == stride;
    };
    if (!sameOuterAndStride(firstRowAddress) ||
        !sameOuterAndStride(secondRowAddress) ||
        !sameOuterAndStride(outputRowAddress)) {
      continue;
    }

    std::vector<MachineOperand> operands{
        j,
        k,
        predicateRowLoad.result,
        productRowLoad.result,
        firstRowAddress->result,
        secondRowAddress->result,
        firstColumn.result,
        secondColumn.result,
        firstColumn.operands[0],
        secondColumn.operands[0],
        outputRowAddress->result,
        MachineOperand::immediateInt(extent),
        MachineOperand::immediateInt(stride, ValueType::int64Type()),
        MachineOperand::label(middleEnd->name),
        MachineOperand::immediateInt(static_cast<std::int64_t>(nextSite),
                                     ValueType::int64Type())};
    row.instructions[entryBranchIndex] = MachineInstruction::target(
        "aarch64.neon.conditional.matrix.reduction", std::move(operands));
    ++nextSite;
  }
  return nextSite;
}

}  // namespace

void AArch64PostRAVectorization::run(
    compiler::backend::mir::MachineModule& module) const {
  if (module.target.name() != "aarch64") return;
  std::size_t nextSite = 0;
  for (auto& function : module.functions) {
    nextSite = migrateConstantFills(function, nextSite);
    nextSite = migrateConditionalMatrixReductions(function, nextSite);
  }
}

}  // namespace compiler::backend::opt
