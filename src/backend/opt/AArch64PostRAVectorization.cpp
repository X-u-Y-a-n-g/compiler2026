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

const MachineInstruction* singleMeaningful(const MachineBlock& block) {
  const auto positions = meaningfulInstructions(block);
  return positions.size() == 1 ? &block.instructions[positions[0]] : nullptr;
}

bool isZeroCopy(const MachineInstruction& instruction,
                const MachineOperand* expected = nullptr) {
  return instruction.opcode == "copy" && instruction.hasResult &&
         instruction.operands.size() == 1 && isImmediate(instruction.operands[0], 0) &&
         (expected == nullptr || sameRegister(instruction.result, *expected));
}

bool isIncrement(const MachineInstruction& instruction,
                 const MachineOperand& induction) {
  return instruction.opcode == "add" && instruction.hasResult &&
         instruction.operands.size() == 2 &&
         sameRegister(instruction.result, induction) &&
         sameRegister(instruction.operands[0], induction) &&
         isImmediate(instruction.operands[1], 1);
}

std::size_t migrateGuardedStencilConvolutions(MachineFunction& function,
                                              std::size_t nextSite) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_STENCIL_INTERIOR_VECTORIZATION") !=
      nullptr) return nextSite;
  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t i = 0; i < function.blocks.size(); ++i)
    indices.emplace(function.blocks[i].name, i);
  const auto blockFor = [&](const MachineOperand& operand) -> MachineBlock* {
    if (!isLabel(operand)) return nullptr;
    const auto found = indices.find(operand.text);
    return found == indices.end() ? nullptr : &function.blocks[found->second];
  };
  const auto twoInstructionLatch = [&](MachineBlock* block,
                                       const MachineOperand& induction,
                                       const MachineBlock& header) {
    if (block == nullptr) return false;
    const auto positions = meaningfulInstructions(*block);
    return positions.size() == 2 &&
           isIncrement(block->instructions[positions[0]], induction) &&
           isBranchTo(block->instructions[positions[1]], header.name);
  };

  for (auto& outputBlock : function.blocks) {
    const auto outputPositions = meaningfulInstructions(outputBlock);
    if (outputPositions.size() != 4) continue;
    const auto& outputIndex = outputBlock.instructions[outputPositions[0]];
    const auto& outputStore = outputBlock.instructions[outputPositions[1]];
    const auto& incrementColumn = outputBlock.instructions[outputPositions[2]];
    const auto& backToColumn = outputBlock.instructions[outputPositions[3]];
    if (outputIndex.opcode != "madd" || !outputIndex.hasResult ||
        outputIndex.operands.size() != 3 || outputStore.opcode != "store.indexed" ||
        outputStore.operands.size() != 4 ||
        !sameRegister(outputStore.operands[2], outputIndex.result) ||
        !isImmediate(outputStore.operands[3], 4) ||
        !isIncrement(incrementColumn, outputIndex.operands[2]) ||
        backToColumn.opcode != "br" || backToColumn.operands.size() != 1) continue;
    const MachineOperand row = outputIndex.operands[0];
    const MachineOperand bound = outputIndex.operands[1];
    const MachineOperand column = outputIndex.operands[2];
    const MachineOperand sum = outputStore.operands[0];
    const MachineOperand outputBase = outputStore.operands[1];
    auto* columnHeader = blockFor(backToColumn.operands[0]);
    const auto* columnCondition = columnHeader ? singleMeaningful(*columnHeader) : nullptr;
    if (!isPhysical(row) || !isPhysical(bound) || !isPhysical(column) ||
        !isPhysical(sum) || !isPhysical(outputBase) || columnCondition == nullptr ||
        columnCondition->opcode != "br.icmp.slt" ||
        columnCondition->operands.size() != 4 ||
        !sameRegister(columnCondition->operands[0], column) ||
        !sameRegister(columnCondition->operands[1], bound)) continue;
    auto* columnBody = blockFor(columnCondition->operands[2]);
    auto* rowLatch = blockFor(columnCondition->operands[3]);
    if (columnBody == nullptr || rowLatch == nullptr) continue;
    const auto columnBodyPositions = meaningfulInstructions(*columnBody);
    if (columnBodyPositions.size() != 3) continue;
    const auto& sumInit = columnBody->instructions[columnBodyPositions[0]];
    const auto& krInit = columnBody->instructions[columnBodyPositions[1]];
    const auto& enterKr = columnBody->instructions[columnBodyPositions[2]];
    if (!isZeroCopy(sumInit, &sum) || !isZeroCopy(krInit) ||
        enterKr.opcode != "br" || enterKr.operands.size() != 1) continue;
    const MachineOperand kr = krInit.result;
    auto* krHeader = blockFor(enterKr.operands[0]);
    const auto* krCondition = krHeader ? singleMeaningful(*krHeader) : nullptr;
    if (krCondition == nullptr || krCondition->opcode != "br.icmp.slt" ||
        krCondition->operands.size() != 4 ||
        !sameRegister(krCondition->operands[0], kr) ||
        krCondition->operands[1].kind != MachineOperandKind::ImmediateInt ||
        blockFor(krCondition->operands[3]) != &outputBlock) continue;
    const long long kernelExtent = krCondition->operands[1].intValue;
    if (kernelExtent <= 0 || kernelExtent > 255) continue;
    auto* krBody = blockFor(krCondition->operands[2]);
    if (krBody == nullptr) continue;
    const auto krBodyPositions = meaningfulInstructions(*krBody);
    if (krBodyPositions.size() != 7) continue;
    const auto& kcInit = krBody->instructions[krBodyPositions[0]];
    const auto& rrAdd = krBody->instructions[krBodyPositions[1]];
    const auto& rrSub = krBody->instructions[krBodyPositions[2]];
    const auto& rrLow = krBody->instructions[krBodyPositions[3]];
    const auto& rrHigh = krBody->instructions[krBodyPositions[4]];
    const auto& rrAnd = krBody->instructions[krBodyPositions[5]];
    const auto& rrBranch = krBody->instructions[krBodyPositions[6]];
    if (!isZeroCopy(kcInit) || rrAdd.opcode != "add" || !rrAdd.hasResult ||
        rrAdd.operands.size() != 2 || !sameRegister(rrAdd.operands[0], row) ||
        !sameRegister(rrAdd.operands[1], kr) || rrSub.opcode != "sub" ||
        !rrSub.hasResult || rrSub.operands.size() != 2 ||
        !sameRegister(rrSub.operands[0], rrAdd.result) ||
        rrSub.operands[1].kind != MachineOperandKind::ImmediateInt ||
        rrLow.opcode != "icmp.sge" || rrLow.operands.size() != 2 ||
        !sameRegister(rrLow.operands[0], rrSub.result) || !isImmediate(rrLow.operands[1], 0) ||
        rrHigh.opcode != "icmp.slt" || rrHigh.operands.size() != 2 ||
        !sameRegister(rrHigh.operands[0], rrSub.result) ||
        !sameRegister(rrHigh.operands[1], bound) || rrAnd.opcode != "and" ||
        rrAnd.operands.size() != 2 || !sameRegister(rrAnd.operands[0], rrLow.result) ||
        !sameRegister(rrAnd.operands[1], rrHigh.result) || rrBranch.opcode != "br" ||
        rrBranch.operands.size() != 3 ||
        !sameRegister(rrBranch.operands[0], rrAnd.result)) continue;
    const long long padding = rrSub.operands[1].intValue;
    if (padding < 0 || padding >= kernelExtent) continue;
    const MachineOperand kc = kcInit.result;
    auto* kcHeader = blockFor(rrBranch.operands[1]);
    auto* krLatch = blockFor(rrBranch.operands[2]);
    if (kcHeader == nullptr || krLatch == nullptr) continue;
    const auto* kcCondition = kcHeader ? singleMeaningful(*kcHeader) : nullptr;
    if (kcCondition == nullptr || kcCondition->opcode != "br.icmp.slt" ||
        kcCondition->operands.size() != 4 ||
        !sameRegister(kcCondition->operands[0], kc) ||
        !isImmediate(kcCondition->operands[1], kernelExtent) ||
        blockFor(kcCondition->operands[3]) != krLatch) continue;
    auto* kcBody = blockFor(kcCondition->operands[2]);
    if (kcBody == nullptr) continue;
    const auto kcBodyPositions = meaningfulInstructions(*kcBody);
    if (kcBodyPositions.size() != 3) continue;
    const auto& ccAdd = kcBody->instructions[kcBodyPositions[0]];
    const auto& ccSub = kcBody->instructions[kcBodyPositions[1]];
    const auto& enterGuard = kcBody->instructions[kcBodyPositions[2]];
    if (ccAdd.opcode != "add" || !ccAdd.hasResult || ccAdd.operands.size() != 2 ||
        !sameRegister(ccAdd.operands[0], column) ||
        !sameRegister(ccAdd.operands[1], kc) || ccSub.opcode != "sub" ||
        !ccSub.hasResult || ccSub.operands.size() != 2 ||
        !sameRegister(ccSub.operands[0], ccAdd.result) ||
        !isImmediate(ccSub.operands[1], padding) || enterGuard.opcode != "br" ||
        enterGuard.operands.size() != 1) continue;
    auto* lowGuard = blockFor(enterGuard.operands[0]);
    const auto* lowCondition = lowGuard ? singleMeaningful(*lowGuard) : nullptr;
    if (lowCondition == nullptr || lowCondition->opcode != "br.icmp.sge" ||
        lowCondition->operands.size() != 4 ||
        !sameRegister(lowCondition->operands[0], ccSub.result) ||
        !isImmediate(lowCondition->operands[1], 0)) continue;
    auto* highGuard = blockFor(lowCondition->operands[2]);
    auto* kcLatch = blockFor(lowCondition->operands[3]);
    if (highGuard == nullptr || kcLatch == nullptr) continue;
    const auto* highCondition = highGuard ? singleMeaningful(*highGuard) : nullptr;
    if (highCondition == nullptr || highCondition->opcode != "br.icmp.slt" ||
        highCondition->operands.size() != 4 ||
        !sameRegister(highCondition->operands[0], ccSub.result) ||
        !sameRegister(highCondition->operands[1], bound) ||
        blockFor(highCondition->operands[3]) != kcLatch) continue;
    auto* tap = blockFor(highCondition->operands[2]);
    if (tap == nullptr) continue;
    const auto tapPositions = meaningfulInstructions(*tap);
    if (tapPositions.size() != 6) continue;
    const auto& inIndex = tap->instructions[tapPositions[0]];
    const auto& inLoad = tap->instructions[tapPositions[1]];
    const auto& kIndex = tap->instructions[tapPositions[2]];
    const auto& kLoad = tap->instructions[tapPositions[3]];
    const auto& madd = tap->instructions[tapPositions[4]];
    const auto& leaveTap = tap->instructions[tapPositions[5]];
    const bool productsMatch =
        madd.operands.size() == 3 &&
        ((sameRegister(madd.operands[0], inLoad.result) &&
          sameRegister(madd.operands[1], kLoad.result)) ||
         (sameRegister(madd.operands[1], inLoad.result) &&
          sameRegister(madd.operands[0], kLoad.result)));
    if (inIndex.opcode != "madd" || !inIndex.hasResult ||
        inIndex.operands.size() != 3 ||
        !sameRegister(inIndex.operands[0], rrSub.result) ||
        !sameRegister(inIndex.operands[1], bound) ||
        !sameRegister(inIndex.operands[2], ccSub.result) ||
        inLoad.opcode != "load.indexed" || !inLoad.hasResult ||
        inLoad.operands.size() != 3 || !sameRegister(inLoad.operands[1], inIndex.result) ||
        !isImmediate(inLoad.operands[2], 4) || kIndex.opcode != "madd" ||
        !kIndex.hasResult || kIndex.operands.size() != 3 ||
        !sameRegister(kIndex.operands[0], kr) ||
        !isImmediate(kIndex.operands[1], kernelExtent) ||
        !sameRegister(kIndex.operands[2], kc) || kLoad.opcode != "load.indexed" ||
        !kLoad.hasResult || kLoad.operands.size() != 3 ||
        !sameRegister(kLoad.operands[1], kIndex.result) ||
        !isImmediate(kLoad.operands[2], 4) || madd.opcode != "madd" ||
        !madd.hasResult || !sameRegister(madd.result, sum) || !productsMatch ||
        !sameRegister(madd.operands[2], sum) ||
        !isBranchTo(leaveTap, kcLatch->name) ||
        !twoInstructionLatch(kcLatch, kc, *kcHeader) ||
        !twoInstructionLatch(krLatch, kr, *krHeader)) continue;
    const MachineOperand inputBase = inLoad.operands[0];
    const MachineOperand kernelBase = kLoad.operands[0];
    if (!isPhysical(inputBase) || !isPhysical(kernelBase)) continue;

    const auto rowLatchPositions = meaningfulInstructions(*rowLatch);
    if (rowLatchPositions.size() != 2 ||
        !isIncrement(rowLatch->instructions[rowLatchPositions[0]], row) ||
        rowLatch->instructions[rowLatchPositions[1]].opcode != "br" ||
        rowLatch->instructions[rowLatchPositions[1]].operands.size() != 1) continue;
    auto* rowHeader = blockFor(rowLatch->instructions[rowLatchPositions[1]].operands[0]);
    const auto* rowCondition = rowHeader ? singleMeaningful(*rowHeader) : nullptr;
    if (rowCondition == nullptr || rowCondition->opcode != "br.icmp.slt" ||
        rowCondition->operands.size() != 4 ||
        !sameRegister(rowCondition->operands[0], row) ||
        !sameRegister(rowCondition->operands[1], bound)) continue;
    auto* columnInit = blockFor(rowCondition->operands[2]);
    auto* repeatLatch = blockFor(rowCondition->operands[3]);
    if (columnInit == nullptr || repeatLatch == nullptr) continue;
    const auto columnInitPositions = meaningfulInstructions(*columnInit);
    if (columnInitPositions.size() != 2 ||
        !isZeroCopy(columnInit->instructions[columnInitPositions[0]], &column) ||
        !isBranchTo(columnInit->instructions[columnInitPositions[1]], columnHeader->name)) continue;
    const auto repeatLatchPositions = meaningfulInstructions(*repeatLatch);
    if (repeatLatchPositions.size() != 2 ||
        repeatLatch->instructions[repeatLatchPositions[1]].opcode != "br" ||
        repeatLatch->instructions[repeatLatchPositions[1]].operands.size() != 1) continue;
    auto* repeatHeader = blockFor(repeatLatch->instructions[repeatLatchPositions[1]].operands[0]);
    const auto* repeatCondition = repeatHeader ? singleMeaningful(*repeatHeader) : nullptr;
    if (repeatCondition == nullptr || repeatCondition->opcode != "br.icmp.slt" ||
        repeatCondition->operands.size() != 4) continue;
    const MachineOperand repeat = repeatCondition->operands[0];
    const MachineOperand trip = repeatCondition->operands[1];
    if (!isPhysical(trip) || !isPhysical(repeat) ||
        !isIncrement(repeatLatch->instructions[repeatLatchPositions[0]], repeat)) continue;
    auto* rowInit = blockFor(repeatCondition->operands[2]);
    auto* exit = blockFor(repeatCondition->operands[3]);
    if (rowInit == nullptr || exit == nullptr) continue;
    const auto rowInitPositions = meaningfulInstructions(*rowInit);
    if (rowInitPositions.size() != 2 ||
        !isZeroCopy(rowInit->instructions[rowInitPositions[0]], &row) ||
        !isBranchTo(rowInit->instructions[rowInitPositions[1]], rowHeader->name)) continue;
    std::optional<std::pair<MachineBlock*, std::size_t>> preheader;
    for (auto& block : function.blocks) {
      if (&block == repeatLatch) continue;
      const auto positions = meaningfulInstructions(block);
      if (positions.empty() ||
          !isBranchTo(block.instructions[positions.back()], repeatHeader->name)) continue;
      bool initializes = false;
      for (std::size_t p = 0; p + 1 < positions.size(); ++p)
        initializes |= isZeroCopy(block.instructions[positions[p]], &repeat);
      if (!initializes || preheader.has_value()) { preheader.reset(); break; }
      preheader = std::pair{&block, positions.back()};
    }
    if (!preheader.has_value()) continue;
    preheader->first->instructions[preheader->second] = MachineInstruction::target(
        "aarch64.neon.stencil.i32x4",
        {inputBase, outputBase, kernelBase, bound, trip, repeat,
         MachineOperand::label(repeatHeader->name), MachineOperand::label(exit->name),
         MachineOperand::immediateInt(kernelExtent), MachineOperand::immediateInt(padding),
         MachineOperand::immediateInt(static_cast<std::int64_t>(nextSite),
                                      ValueType::int64Type())});
    return nextSite + 1;
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
    nextSite = migrateGuardedStencilConvolutions(function, nextSite);
  }
}

}  // namespace compiler::backend::opt
