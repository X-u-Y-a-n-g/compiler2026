#include "../../../include/compiler/backend/opt/AArch64InstructionCombining.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <string>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/AArch64MagicDivision.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {
namespace {

using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::mir::ValueType;

struct NaturalLoop {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::unordered_set<std::size_t> blocks;
};

struct ConstantDivisionCandidate {
  std::size_t block = 0;
  std::size_t instruction = 0;
  std::int32_t divisor = 0;
  compiler::backend::target::aarch64::SignedMagic32 magic;
  int adjustment = 0;
  bool isRemainder = false;
  unsigned estimatedSaving = 0;
};

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

std::vector<bool> computeReachable(
    const compiler::backend::common::ControlFlowGraph& cfg) {
  std::vector<bool> reachable(cfg.blocks.size(), false);
  if (cfg.blocks.empty()) return reachable;
  std::deque<std::size_t> worklist;
  reachable[0] = true;
  worklist.push_back(0);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto successor : cfg.successors[block]) {
      if (!reachable[successor]) {
        reachable[successor] = true;
        worklist.push_back(successor);
      }
    }
  }
  return reachable;
}

std::vector<std::unordered_set<std::size_t>> computeDominators(
    const compiler::backend::common::ControlFlowGraph& cfg,
    const std::vector<bool>& reachable) {
  std::unordered_set<std::size_t> allReachable;
  for (std::size_t block = 0; block < reachable.size(); ++block) {
    if (reachable[block]) allReachable.insert(block);
  }
  std::vector<std::unordered_set<std::size_t>> dominators(reachable.size());
  if (reachable.empty()) return dominators;
  dominators[0].insert(0);
  for (std::size_t block = 1; block < reachable.size(); ++block) {
    if (reachable[block]) dominators[block] = allReachable;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < reachable.size(); ++block) {
      if (!reachable[block]) continue;
      std::optional<std::unordered_set<std::size_t>> intersection;
      for (const auto predecessor : cfg.predecessors[block]) {
        if (!reachable[predecessor]) continue;
        if (!intersection.has_value()) {
          intersection = dominators[predecessor];
          continue;
        }
        std::unordered_set<std::size_t> next;
        for (const auto candidate : *intersection) {
          if (dominators[predecessor].find(candidate) !=
              dominators[predecessor].end()) {
            next.insert(candidate);
          }
        }
        intersection = std::move(next);
      }
      auto next = intersection.value_or(std::unordered_set<std::size_t>{});
      next.insert(block);
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

std::vector<NaturalLoop> findNaturalLoops(
    const compiler::backend::common::ControlFlowGraph& cfg,
    const std::vector<bool>& reachable,
    const std::vector<std::unordered_set<std::size_t>>& dominators) {
  std::unordered_map<std::size_t, NaturalLoop> byHeader;
  for (std::size_t latch = 0; latch < cfg.blocks.size(); ++latch) {
    if (!reachable[latch]) continue;
    for (const auto header : cfg.successors[latch]) {
      if (dominators[latch].find(header) == dominators[latch].end()) continue;
      std::unordered_set<std::size_t> blocks{header, latch};
      std::deque<std::size_t> worklist;
      if (latch != header) worklist.push_back(latch);
      while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop_front();
        for (const auto predecessor : cfg.predecessors[block]) {
          if (blocks.insert(predecessor).second && predecessor != header) {
            worklist.push_back(predecessor);
          }
        }
      }
      auto& loop = byHeader[header];
      loop.header = header;
      loop.blocks.insert(blocks.begin(), blocks.end());
    }
  }

  std::vector<NaturalLoop> loops;
  for (auto& [header, loop] : byHeader) {
    std::optional<std::size_t> preheader;
    bool unique = true;
    for (const auto predecessor : cfg.predecessors[header]) {
      if (loop.blocks.find(predecessor) != loop.blocks.end()) continue;
      if (preheader.has_value()) {
        unique = false;
        break;
      }
      preheader = predecessor;
    }
    if (!unique || !preheader.has_value() ||
        cfg.successors[*preheader] != std::vector<std::size_t>{header}) {
      continue;
    }
    loop.preheader = *preheader;
    loops.push_back(std::move(loop));
  }
  std::sort(loops.begin(), loops.end(),
            [](const NaturalLoop& lhs, const NaturalLoop& rhs) {
              return lhs.blocks.size() < rhs.blocks.size();
            });
  return loops;
}

bool isPowerOfTwoMagnitude(std::int64_t value) {
  if (value == 0) return false;
  const auto magnitude =
      value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                : static_cast<std::uint64_t>(value);
  return (magnitude & (magnitude - 1U)) == 0;
}

std::optional<ConstantDivisionCandidate> analyzeConstantDivision(
    const MachineInstruction& instruction, std::size_t block,
    std::size_t instructionIndex, std::optional<std::int32_t> knownDivisor) {
  const bool division = instruction.opcode == "div" ||
                        instruction.opcode == "sdiv";
  const bool remainder = instruction.opcode == "mod" ||
                         instruction.opcode == "srem";
  if ((!division && !remainder) || !instruction.hasResult ||
      !instruction.result.type.isInt32() || instruction.operands.size() != 2) {
    return std::nullopt;
  }
  if (!knownDivisor.has_value()) {
    return std::nullopt;
  }
  const auto divisor = *knownDivisor;
  if (divisor == 0 || isPowerOfTwoMagnitude(divisor)) return std::nullopt;
  const auto magic =
      compiler::backend::target::aarch64::signedMagic32(divisor);
  if (!magic.has_value()) return std::nullopt;

  int adjustment = 0;
  if (divisor > 0 && magic->multiplier < 0) {
    adjustment = 1;
  } else if (divisor < 0 && magic->multiplier > 0) {
    adjustment = -1;
  }

  // Cortex-A53 integer SDIV is a long, non-pipelined dependency.  The magic
  // quotient needs multiply-high, sign extraction, optional adjustment/shift,
  // and one truncation correction.  Remainder adds one MSUB.  Require at least
  // four estimated cycles of recurring-loop benefit before increasing
  // invariant register pressure.
  const unsigned loweredCost = 3U + (adjustment != 0 ? 1U : 0U) +
                               (magic->shift != 0 ? 1U : 0U) +
                               (remainder ? 1U : 0U);
  const unsigned hardwareCost = remainder ? 13U : 12U;
  if (loweredCost + 4U > hardwareCost) return std::nullopt;
  return ConstantDivisionCandidate{block, instructionIndex, divisor, *magic,
                                   adjustment, remainder,
                                   hardwareCost - loweredCost};
}

std::optional<std::int32_t> resolveLoopInvariantI32Constant(
    const MachineOperand& operand, const NaturalLoop& loop,
    const std::unordered_map<std::string, Definition>& definitions,
    const MachineFunction& function, std::unordered_set<std::string>& visiting) {
  if (operand.kind == MachineOperandKind::ImmediateInt) {
    if (operand.intValue < std::numeric_limits<std::int32_t>::min() ||
        operand.intValue > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::int32_t>(operand.intValue);
  }
  if (!isVirtualRegister(operand) || !visiting.insert(operand.text).second) {
    return std::nullopt;
  }
  const auto found = definitions.find(operand.text);
  if (found == definitions.end() ||
      loop.blocks.find(found->second.block) != loop.blocks.end()) {
    visiting.erase(operand.text);
    return std::nullopt;
  }
  const auto& definition =
      function.blocks[found->second.block].instructions[found->second.instruction];
  std::optional<std::int32_t> result;
  if (definition.opcode == "copy" && definition.operands.size() == 1) {
    result = resolveLoopInvariantI32Constant(definition.operands[0], loop,
                                             definitions, function, visiting);
  } else if (definition.opcode == "neg" &&
             definition.operands.size() == 1) {
    if (const auto value = resolveLoopInvariantI32Constant(
            definition.operands[0], loop, definitions, function, visiting)) {
      const auto bits = 0U - static_cast<std::uint32_t>(*value);
      std::int32_t negated = 0;
      std::memcpy(&negated, &bits, sizeof(negated));
      result = negated;
    }
  }
  visiting.erase(operand.text);
  return result;
}

std::size_t terminatorInsertionPoint(
    const compiler::backend::mir::MachineBlock& block) {
  std::size_t position = block.instructions.size();
  while (position > 0) {
    const auto& opcode = block.instructions[position - 1].opcode;
    if (opcode != "br" && opcode.rfind("br.icmp.", 0) != 0 &&
        opcode != "ret") {
      break;
    }
    --position;
  }
  return position;
}

void lowerFunctionHotConstantDivisions(
    MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::size_t instructionCount = 0;
  for (const auto& block : function.blocks) {
    instructionCount += block.instructions.size();
  }
  if (instructionCount > 4096) return;

  const auto cfg = compiler::backend::common::CFGBuilder{}.build(function,
                                                                  adapter);
  const auto reachable = computeReachable(cfg);
  const auto dominators = computeDominators(cfg, reachable);
  const auto loops = findNaturalLoops(cfg, reachable, dominators);
  auto names = collectVirtualRegisterNames(function, adapter);
  std::unordered_map<std::string, Definition> definitions;
  const auto rebuildDefinitions = [&]() {
    definitions.clear();
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
      for (std::size_t instruction = 0;
           instruction < function.blocks[block].instructions.size();
           ++instruction) {
        const auto& current = function.blocks[block].instructions[instruction];
        if (current.hasResult && isVirtualRegister(current.result)) {
          definitions.emplace(current.result.text,
                              Definition{block, instruction});
        }
      }
    }
  };
  rebuildDefinitions();

  for (const auto& loop : loops) {
    bool hasCall = false;
    std::vector<ConstantDivisionCandidate> candidates;
    for (const auto blockIndex : loop.blocks) {
      const auto& instructions = function.blocks[blockIndex].instructions;
      for (std::size_t instructionIndex = 0;
           instructionIndex < instructions.size(); ++instructionIndex) {
        if (instructions[instructionIndex].opcode == "call") hasCall = true;
        std::unordered_set<std::string> visiting;
        const auto knownDivisor =
            instructions[instructionIndex].operands.size() == 2
                ? resolveLoopInvariantI32Constant(
                      instructions[instructionIndex].operands[1], loop,
                      definitions, function, visiting)
                : std::nullopt;
        if (const auto candidate = analyzeConstantDivision(
                instructions[instructionIndex], blockIndex, instructionIndex,
                knownDivisor)) {
          candidates.push_back(*candidate);
        }
      }
    }
    if (hasCall || candidates.empty()) continue;

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& lhs, const auto& rhs) {
                if (lhs.estimatedSaving != rhs.estimatedSaving) {
                  return lhs.estimatedSaving > rhs.estimatedSaving;
                }
                if (lhs.block != rhs.block) return lhs.block < rhs.block;
                return lhs.instruction < rhs.instruction;
              });

    // Avoid turning an unusually constant-heavy loop into a register-pressure
    // regression.  This is a target resource limit, not a workload identity.
    constexpr std::size_t kMaximumNewInvariantRegisters = 8;
    std::unordered_set<std::int32_t> selectedConstants;
    std::vector<ConstantDivisionCandidate> selected;
    for (const auto& candidate : candidates) {
      auto required = selectedConstants;
      required.insert(candidate.magic.multiplier);
      if (candidate.isRemainder) required.insert(candidate.divisor);
      if (required.size() > kMaximumNewInvariantRegisters) continue;
      selectedConstants = std::move(required);
      selected.push_back(candidate);
    }
    if (selected.empty()) continue;
    unsigned recurringSaving = 0;
    for (const auto& candidate : selected) {
      recurringSaving += candidate.estimatedSaving;
    }
    // A lone quotient replacement is especially sensitive to code layout and
    // invariant-register pressure.  Require enough aggregate recurring-loop
    // benefit to pay for both effects; serial div/rem chains clear this bar,
    // while isolated operations stay on the compact hardware SDIV path.
    constexpr unsigned kMinimumAggregateRecurringSaving = 12;
    if (recurringSaving < kMinimumAggregateRecurringSaving) continue;

    std::unordered_map<std::int32_t, MachineOperand> constants;
    std::vector<MachineInstruction> initializers;
    auto constantRegister = [&](std::int32_t value) -> MachineOperand {
      const auto found = constants.find(value);
      if (found != constants.end()) return found->second;
      const auto name = freshVirtualRegisterName(
          names, "%aarch64.hot.div.constant");
      auto operand = MachineOperand::virtualRegister(name,
                                                     ValueType::int32Type());
      initializers.push_back(MachineInstruction::copy(
          operand, MachineOperand::immediateInt(value, ValueType::int32Type())));
      constants.emplace(value, operand);
      return operand;
    };

    for (const auto& candidate : selected) {
      auto& instruction = function.blocks[candidate.block]
                              .instructions[candidate.instruction];
      if (instruction.opcode != "div" && instruction.opcode != "sdiv" &&
          instruction.opcode != "mod" && instruction.opcode != "srem") {
        continue;
      }
      const auto multiplier = constantRegister(candidate.magic.multiplier);
      if (candidate.isRemainder) {
        const auto divisor = constantRegister(candidate.divisor);
        instruction = MachineInstruction::target(
            "srem.magic.i32", instruction.result,
            {instruction.operands[0], multiplier, divisor,
             MachineOperand::immediateInt(candidate.adjustment,
                                          ValueType::int32Type()),
             MachineOperand::immediateInt(candidate.magic.shift,
                                          ValueType::int32Type())});
      } else {
        instruction = MachineInstruction::target(
            "sdiv.magic.i32", instruction.result,
            {instruction.operands[0], multiplier,
             MachineOperand::immediateInt(candidate.adjustment,
                                          ValueType::int32Type()),
             MachineOperand::immediateInt(candidate.magic.shift,
                                          ValueType::int32Type())});
      }
    }

    auto& preheader = function.blocks[loop.preheader];
    const auto position = terminatorInsertionPoint(preheader);
    preheader.instructions.insert(preheader.instructions.begin() + position,
                                  initializers.begin(), initializers.end());
    rebuildDefinitions();
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

void AArch64InstructionCombining::lowerHotConstantDivisions(
    compiler::backend::mir::MachineModule& module) const {
  if (module.target.name() != "aarch64" ||
      std::getenv("COMPILER_DISABLE_AARCH64_HOT_MAGIC_DIVISION") != nullptr) {
    return;
  }
  const auto& adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  for (auto& function : module.functions) {
    lowerFunctionHotConstantDivisions(function, adapter);
  }
}

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
  for (auto& function : module.functions) {
    combinePostRAPairedI32Loads(function);
    combinePostRACountedBranches(function);
    redirectPostRANoOpBlocks(function);
  }
}

}  // namespace compiler::backend::opt
