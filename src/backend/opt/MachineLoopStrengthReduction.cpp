#include "../../../include/compiler/backend/opt/MachineLoopStrengthReduction.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {

namespace {

constexpr std::size_t kMaxAnalyzedInstructions = 4096;

using compiler::backend::common::CFGBuilder;
using compiler::backend::common::ControlFlowGraph;
using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

struct LoopInfo {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::unordered_set<std::size_t> blocks;
  std::vector<std::size_t> latches;
};

struct Definition {
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Induction {
  MachineOperand phi;
  MachineOperand initial;
  long long step = 1;
};

struct Candidate {
  std::size_t block = 0;
  std::size_t instruction = 0;
  MachineInstruction gep;
  long long inductionOffset = 0;
  unsigned estimatedSaving = 0;
  bool hoistStride = false;
  bool requiresAdjacentPartner = false;
};

struct Plan {
  LoopInfo loop;
  Induction induction;
  std::vector<Candidate> candidates;
};

bool isVirtualRegister(const MachineOperand &operand) {
  return operand.kind == MachineOperandKind::VirtualRegister;
}

bool sameOperand(const MachineOperand &lhs, const MachineOperand &rhs) {
  if (lhs.kind != rhs.kind || lhs.type.kind != rhs.type.kind) {
    return false;
  }
  switch (lhs.kind) {
  case MachineOperandKind::VirtualRegister:
  case MachineOperandKind::PhysicalRegister:
  case MachineOperandKind::GlobalAddress:
  case MachineOperandKind::FunctionSymbol:
  case MachineOperandKind::Label:
    return lhs.text == rhs.text;
  case MachineOperandKind::ImmediateInt:
    return lhs.intValue == rhs.intValue;
  case MachineOperandKind::ImmediateFloat:
    return lhs.floatValue == rhs.floatValue;
  case MachineOperandKind::FrameIndex:
  case MachineOperandKind::StackSlot:
  case MachineOperandKind::StackObject:
    return lhs.index == rhs.index;
  case MachineOperandKind::None:
    return true;
  }
  return false;
}

bool isGep(const MachineInstruction &instruction) {
  return (instruction.opcode == "gep" || instruction.opcode == "gep.linear" ||
          instruction.opcode == "getelementptr" ||
          instruction.opcode == "getelementptr.linear") &&
         instruction.hasResult && instruction.operands.size() == 3 &&
         instruction.operands[2].kind == MachineOperandKind::ImmediateInt;
}

std::vector<bool> computeReachable(const ControlFlowGraph &cfg) {
  std::vector<bool> reachable(cfg.blocks.size(), false);
  if (cfg.blocks.empty()) {
    return reachable;
  }
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

std::vector<std::unordered_set<std::size_t>>
computeDominators(const ControlFlowGraph &cfg,
                  const std::vector<bool> &reachable) {
  std::unordered_set<std::size_t> allReachable;
  for (std::size_t block = 0; block < reachable.size(); ++block) {
    if (reachable[block]) {
      allReachable.insert(block);
    }
  }
  std::vector<std::unordered_set<std::size_t>> dominators(reachable.size());
  if (reachable.empty()) {
    return dominators;
  }
  dominators[0].insert(0);
  for (std::size_t block = 1; block < reachable.size(); ++block) {
    if (reachable[block]) {
      dominators[block] = allReachable;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < reachable.size(); ++block) {
      if (!reachable[block]) {
        continue;
      }
      std::optional<std::unordered_set<std::size_t>> intersection;
      for (const auto predecessor : cfg.predecessors[block]) {
        if (!reachable[predecessor]) {
          continue;
        }
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

std::vector<LoopInfo>
findLoops(const ControlFlowGraph &cfg, const std::vector<bool> &reachable,
          const std::vector<std::unordered_set<std::size_t>> &dominators) {
  std::unordered_map<std::size_t, LoopInfo> byHeader;
  for (std::size_t latch = 0; latch < cfg.blocks.size(); ++latch) {
    if (!reachable[latch]) {
      continue;
    }
    for (const auto header : cfg.successors[latch]) {
      if (dominators[latch].find(header) == dominators[latch].end()) {
        continue;
      }
      std::unordered_set<std::size_t> naturalLoop{header, latch};
      std::deque<std::size_t> worklist;
      if (latch != header) {
        worklist.push_back(latch);
      }
      while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop_front();
        for (const auto predecessor : cfg.predecessors[block]) {
          if (naturalLoop.insert(predecessor).second && predecessor != header) {
            worklist.push_back(predecessor);
          }
        }
      }
      auto &loop = byHeader[header];
      loop.header = header;
      loop.blocks.insert(naturalLoop.begin(), naturalLoop.end());
      if (std::find(loop.latches.begin(), loop.latches.end(), latch) ==
          loop.latches.end()) {
        loop.latches.push_back(latch);
      }
    }
  }

  std::vector<LoopInfo> loops;
  for (auto &[header, loop] : byHeader) {
    std::optional<std::size_t> preheader;
    bool unique = true;
    for (const auto predecessor : cfg.predecessors[header]) {
      if (loop.blocks.find(predecessor) != loop.blocks.end()) {
        continue;
      }
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
    std::sort(loop.latches.begin(), loop.latches.end());
    loops.push_back(std::move(loop));
  }
  std::sort(loops.begin(), loops.end(),
            [](const LoopInfo &lhs, const LoopInfo &rhs) {
              return lhs.blocks.size() < rhs.blocks.size();
            });
  return loops;
}

std::unordered_map<std::string, Definition>
buildDefinitions(const MachineFunction &function) {
  std::unordered_map<std::string, Definition> definitions;
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    for (std::size_t instruction = 0;
         instruction < function.blocks[block].instructions.size();
         ++instruction) {
      const auto &current = function.blocks[block].instructions[instruction];
      if (current.hasResult && isVirtualRegister(current.result)) {
        definitions.emplace(current.result.text,
                            Definition{block, instruction});
      }
    }
  }
  return definitions;
}

std::unordered_map<std::string, std::vector<std::size_t>>
buildUses(const MachineFunction &function,
          const compiler::backend::target::TargetInstrAdapter &adapter) {
  std::unordered_map<std::string, std::vector<std::size_t>> uses;
  std::vector<MachineOperand> operands;
  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    for (const auto &instruction : function.blocks[block].instructions) {
      adapter.enumerateUses(instruction, operands);
      for (const auto &operand : operands) {
        if (isVirtualRegister(operand)) {
          uses[operand.text].push_back(block);
        }
      }
    }
  }
  return uses;
}

bool isLoopInvariant(
    const MachineOperand &operand, const LoopInfo &loop,
    const std::unordered_map<std::string, Definition> &definitions) {
  if (!isVirtualRegister(operand)) {
    return true;
  }
  const auto definition = definitions.find(operand.text);
  return definition == definitions.end() ||
         loop.blocks.find(definition->second.block) == loop.blocks.end();
}

std::optional<long long> inductionOffsetForIndexImpl(
    const MachineOperand &index, const MachineOperand &induction,
    const LoopInfo &loop,
    const std::unordered_map<std::string, Definition> &definitions,
    const MachineFunction &function, unsigned depth) {
  if (sameOperand(index, induction)) {
    return 0;
  }
  if (!isVirtualRegister(index) || depth == 0) {
    return std::nullopt;
  }
  const auto definition = definitions.find(index.text);
  if (definition == definitions.end() ||
      loop.blocks.find(definition->second.block) == loop.blocks.end()) {
    return std::nullopt;
  }
  const auto &instruction = function.blocks[definition->second.block]
                                .instructions[definition->second.instruction];
  if (instruction.opcode != "add" || instruction.operands.size() != 2) {
    return std::nullopt;
  }
  for (std::size_t immediateIndex = 0; immediateIndex < 2; ++immediateIndex) {
    const auto valueIndex = 1 - immediateIndex;
    const auto &immediate = instruction.operands[immediateIndex];
    if (immediate.kind != MachineOperandKind::ImmediateInt)
      continue;
    const auto base =
        inductionOffsetForIndexImpl(instruction.operands[valueIndex], induction,
                                    loop, definitions, function, depth - 1);
    if (!base.has_value())
      continue;
    const __int128 offset = static_cast<__int128>(*base) + immediate.intValue;
    if (offset < std::numeric_limits<long long>::min() ||
        offset > std::numeric_limits<long long>::max()) {
      return std::nullopt;
    }
    return static_cast<long long>(offset);
  }
  return std::nullopt;
}

std::optional<long long> inductionOffsetForIndex(
    const MachineOperand &index, const MachineOperand &induction,
    const LoopInfo &loop,
    const std::unordered_map<std::string, Definition> &definitions,
    const MachineFunction &function) {
  return inductionOffsetForIndexImpl(index, induction, loop, definitions,
                                     function, 4);
}

std::optional<MachineOperand> phiIncoming(const MachineInstruction &phi,
                                          const std::string &block) {
  std::optional<MachineOperand> result;
  for (const auto &incoming : phi.phiInputs) {
    if (incoming.block != block) {
      continue;
    }
    if (result.has_value()) {
      return std::nullopt;
    }
    result = incoming.value;
  }
  return result;
}

std::optional<Induction>
matchInduction(const MachineFunction &function, const LoopInfo &loop,
               const MachineInstruction &phi,
               const std::unordered_map<std::string, Definition> &definitions) {
  if (phi.kind != MachineInstructionKind::Phi || !phi.hasResult ||
      !isVirtualRegister(phi.result) || !phi.result.type.isInt32()) {
    return std::nullopt;
  }
  if (phi.phiInputs.size() != loop.latches.size() + 1) {
    return std::nullopt;
  }
  std::unordered_set<std::string> expectedIncoming{
      function.blocks[loop.preheader].name};
  for (const auto latch : loop.latches) {
    expectedIncoming.insert(function.blocks[latch].name);
  }
  std::unordered_set<std::string> actualIncoming;
  for (const auto &incoming : phi.phiInputs) {
    actualIncoming.insert(incoming.block);
  }
  if (actualIncoming != expectedIncoming) {
    return std::nullopt;
  }
  const auto initial = phiIncoming(phi, function.blocks[loop.preheader].name);
  if (!initial.has_value()) {
    return std::nullopt;
  }
  std::optional<long long> inductionStep;
  for (const auto latch : loop.latches) {
    const auto next = phiIncoming(phi, function.blocks[latch].name);
    if (!next.has_value() || !isVirtualRegister(*next)) {
      return std::nullopt;
    }
    const auto definition = definitions.find(next->text);
    if (definition == definitions.end() || definition->second.block != latch) {
      return std::nullopt;
    }
    const auto step =
        inductionOffsetForIndex(*next, phi.result, loop, definitions, function);
    if (!step.has_value() ||
        (*step != 1 && *step != 2 && *step != 4) ||
        (inductionStep.has_value() && *inductionStep != *step)) {
      return std::nullopt;
    }
    inductionStep = step;
  }
  if (!inductionStep.has_value())
    return std::nullopt;
  return Induction{phi.result, *initial, *inductionStep};
}

unsigned dynamicGepCost(long long scale) {
  if (scale == 0 || scale == 1) {
    return 1;
  }
  const auto magnitude =
      static_cast<unsigned long long>(scale < 0 ? -scale : scale);
  if ((magnitude & (magnitude - 1)) == 0) {
    return 2;
  }
  return 3;
}

unsigned immediateGepCost(long long scale) {
  if (scale == 0) {
    return 0;
  }
  return scale >= -2048 && scale <= 2047 ? 1 : 2;
}

bool isNonPowerOfTwoScale(long long scale) {
  const auto magnitude = static_cast<unsigned long long>(
      scale < 0 ? -static_cast<__int128>(scale) : scale);
  return magnitude > 1 && (magnitude & (magnitude - 1)) != 0;
}

bool isNestedLoop(const LoopInfo &loop, const std::vector<LoopInfo> &loops) {
  return std::any_of(loops.begin(), loops.end(), [&](const auto &outer) {
    if (loop.blocks.size() >= outer.blocks.size())
      return false;
    return std::all_of(loop.blocks.begin(), loop.blocks.end(),
                       [&](const auto block) {
                         return outer.blocks.find(block) != outer.blocks.end();
                       });
  });
}

bool containsVectorMemoryOperation(const MachineFunction &function,
                                   const LoopInfo &loop) {
  return std::any_of(
      loop.blocks.begin(), loop.blocks.end(), [&](const auto block) {
        return std::any_of(function.blocks[block].instructions.begin(),
                           function.blocks[block].instructions.end(),
                           [](const auto &instruction) {
                             return instruction.opcode.rfind("aarch64.neon.",
                                                             0) == 0;
                           });
      });
}

bool isSimpleReadOnlyMultiplySubReduction(const MachineFunction &function,
                                          const LoopInfo &loop) {
  unsigned loads = 0;
  unsigned multiplies = 0;
  unsigned subtracts = 0;
  for (const auto block : loop.blocks) {
    for (const auto &instruction : function.blocks[block].instructions) {
      if (instruction.opcode == "call" || instruction.opcode == "store" ||
          instruction.opcode == "fstore" ||
          instruction.opcode.rfind("aarch64.neon.", 0) == 0) {
        return false;
      }
      if (instruction.opcode == "load") {
        ++loads;
      } else if (instruction.opcode == "mul") {
        ++multiplies;
      } else if (instruction.opcode == "sub") {
        ++subtracts;
      }
    }
  }
  return loop.blocks.size() == 2 && loads == 2 && multiplies == 1 &&
         subtracts == 1;
}

bool isInterleavedReadOnlyMultiplySubReduction(const MachineFunction &function,
                                               const LoopInfo &loop) {
  unsigned loads = 0;
  unsigned multiplies = 0;
  unsigned subtracts = 0;
  for (const auto block : loop.blocks) {
    for (const auto &instruction : function.blocks[block].instructions) {
      if (instruction.opcode == "call" || instruction.opcode == "store" ||
          instruction.opcode == "fstore" ||
          instruction.opcode.rfind("aarch64.neon.", 0) == 0) {
        return false;
      }
      if (instruction.opcode == "load") {
        ++loads;
      } else if (instruction.opcode == "mul") {
        ++multiplies;
      } else if (instruction.opcode == "sub") {
        ++subtracts;
      }
    }
  }
  return loop.blocks.size() == 2 && loads == 4 && multiplies == 2 &&
         (subtracts == 2 || subtracts == 3);
}

bool isSimpleReadOnlyIntegerSelectReduction(const MachineFunction &function,
                                            const LoopInfo &loop) {
  unsigned loads = 0;
  unsigned multiplies = 0;
  unsigned selects = 0;
  for (const auto block : loop.blocks) {
    for (const auto &instruction : function.blocks[block].instructions) {
      if (instruction.opcode == "call" || instruction.opcode == "store" ||
          instruction.opcode == "fstore" ||
          instruction.opcode.rfind("aarch64.neon.", 0) == 0) {
        return false;
      }
      if (instruction.opcode == "load") {
        ++loads;
      } else if (instruction.opcode == "mul") {
        ++multiplies;
      } else if (instruction.opcode == "select") {
        ++selects;
      }
    }
  }
  return loop.blocks.size() == 2 && loads == 2 && multiplies == 1 &&
         selects == 1;
}

bool isFourWayReadOnlyIntegerSelectReduction(const MachineFunction &function,
                                             const LoopInfo &loop) {
  unsigned loads = 0;
  unsigned multiplies = 0;
  unsigned selects = 0;
  for (const auto block : loop.blocks) {
    for (const auto &instruction : function.blocks[block].instructions) {
      if (instruction.opcode == "call" || instruction.opcode == "store" ||
          instruction.opcode == "fstore" ||
          instruction.opcode.rfind("aarch64.neon.", 0) == 0) {
        return false;
      }
      if (instruction.opcode == "load") {
        ++loads;
      } else if (instruction.opcode == "mul") {
        ++multiplies;
      } else if (instruction.opcode == "select") {
        ++selects;
      }
    }
  }
  return loop.blocks.size() == 2 && loads == 8 && multiplies == 4 &&
         selects == 4;
}

std::optional<Plan>
findPlan(const MachineFunction &function,
         const compiler::backend::target::TargetInstrAdapter &adapter,
         const std::unordered_set<std::string> &transformedHeaders,
         bool enableNestedScalarLargeStride) {
  const auto cfg = CFGBuilder{}.build(function, adapter);
  const auto reachable = computeReachable(cfg);
  const auto dominators = computeDominators(cfg, reachable);
  const auto loops = findLoops(cfg, reachable, dominators);
  const auto definitions = buildDefinitions(function);
  const auto uses = buildUses(function, adapter);

  for (const auto &loop : loops) {
    const bool nested = isNestedLoop(loop, loops);
    const bool vectorMemoryLoop = containsVectorMemoryOperation(function, loop);
    const bool scalarMultiplySubReduction =
        isSimpleReadOnlyMultiplySubReduction(function, loop);
    const bool interleavedMultiplySubReduction =
        isInterleavedReadOnlyMultiplySubReduction(function, loop);
    const bool scalarIntegerSelectReduction =
        isSimpleReadOnlyIntegerSelectReduction(function, loop);
    const bool fourWayIntegerSelectReduction =
        isFourWayReadOnlyIntegerSelectReduction(function, loop);
    const auto &headerName = function.blocks[loop.header].name;
    if (transformedHeaders.find(headerName) != transformedHeaders.end()) {
      continue;
    }
    for (const auto &phi : function.blocks[loop.header].instructions) {
      const auto induction = matchInduction(function, loop, phi, definitions);
      if (!induction.has_value()) {
        continue;
      }
      const bool fourWaySelectLoop =
          fourWayIntegerSelectReduction && induction->step == 4;
      if (induction->step == 4 &&
          (!enableNestedScalarLargeStride || !fourWaySelectLoop ||
           std::getenv(
               "COMPILER_DISABLE_AARCH64_FOUR_WAY_SELECT_POINTER_IV") !=
               nullptr)) {
        continue;
      }
      if (induction->step == 2 &&
          (!enableNestedScalarLargeStride ||
           std::getenv("COMPILER_DISABLE_AARCH64_STEP_TWO_POINTER_IV") !=
               nullptr)) {
        continue;
      }
      std::vector<Candidate> candidates;
      for (const auto block : loop.blocks) {
        bool dominatesEveryLatch = true;
        for (const auto latch : loop.latches) {
          if (dominators[latch].find(block) == dominators[latch].end()) {
            dominatesEveryLatch = false;
            break;
          }
        }
        if (!dominatesEveryLatch) {
          continue;
        }
        for (std::size_t instruction = 0;
             instruction < function.blocks[block].instructions.size();
             ++instruction) {
          const auto &current =
              function.blocks[block].instructions[instruction];
          if (!isGep(current) ||
              !isLoopInvariant(current.operands[0], loop, definitions)) {
            continue;
          }
          const auto inductionOffset = inductionOffsetForIndex(
              current.operands[1], induction->phi, loop, definitions, function);
          if (!inductionOffset.has_value() ||
              (*inductionOffset != 0 && !(enableNestedScalarLargeStride &&
                                           (scalarIntegerSelectReduction ||
                                           interleavedMultiplySubReduction ||
                                           (fourWaySelectLoop &&
                                            *inductionOffset >= 0 &&
                                            *inductionOffset <= 4))))) {
            continue;
          }
          const auto resultUses = uses.find(current.result.text);
          if (resultUses == uses.end() || resultUses->second.empty() ||
              std::any_of(resultUses->second.begin(), resultUses->second.end(),
                          [&](const auto useBlock) {
                            return loop.blocks.find(useBlock) ==
                                   loop.blocks.end();
                          })) {
            continue;
          }
          const auto scale = current.operands[2].intValue;
          const __int128 steppedScale =
              static_cast<__int128>(scale) * induction->step;
          if (steppedScale < std::numeric_limits<long long>::min() ||
              steppedScale > std::numeric_limits<long long>::max()) {
            continue;
          }
          const auto oldCost = dynamicGepCost(scale);
          const auto newCost = immediateGepCost(scale);
          const bool allowNestedVectorLargeStride =
              std::getenv(
                  "COMPILER_DISABLE_NESTED_NON_POWER_OF_TWO_POINTER_IV") ==
                  nullptr &&
              nested && vectorMemoryLoop && isNonPowerOfTwoScale(scale) &&
              oldCost == newCost + 1;
          const bool allowNestedScalarLargeStride =
              enableNestedScalarLargeStride &&
              std::getenv("COMPILER_DISABLE_NESTED_SCALAR_NON_POWER_OF_TWO_"
                          "POINTER_IV") == nullptr &&
              nested &&
              (scalarMultiplySubReduction || interleavedMultiplySubReduction) &&
              isNonPowerOfTwoScale(scale) && oldCost == newCost + 1;
          const bool allowNestedAffineSelectLargeStride =
              enableNestedScalarLargeStride &&
              std::getenv("COMPILER_DISABLE_NESTED_AFFINE_SELECT_POINTER_IV") ==
                  nullptr &&
              nested &&
              (scalarIntegerSelectReduction ||
               fourWaySelectLoop) &&
              isNonPowerOfTwoScale(scale) && oldCost == newCost + 1;
          // A step-two interleaved i32 reduction consumes two adjacent
          // elements from each stream.  Even a power-of-two byte scale is
          // profitable when both GEPs can share one pointer phi and one
          // update, because the second address becomes a fixed +4 offset.
          const bool allowInterleavedAdjacentI32Pointer =
              enableNestedScalarLargeStride &&
              interleavedMultiplySubReduction && induction->step == 2 &&
              (scale == 4 || scale == -4) &&
              (*inductionOffset == 0 || *inductionOffset == 1);
          const bool allowNestedLargeStride =
              allowNestedVectorLargeStride || allowNestedScalarLargeStride ||
              allowNestedAffineSelectLargeStride;
          if (oldCost < newCost + 2 && !allowNestedLargeStride &&
              !allowInterleavedAdjacentI32Pointer) {
            continue;
          }
          candidates.push_back(Candidate{block, instruction, current,
                                         *inductionOffset, oldCost - newCost,
                                         allowNestedLargeStride,
                                         allowInterleavedAdjacentI32Pointer});
        }
      }
      std::vector<Candidate> pairedCandidates;
      pairedCandidates.reserve(candidates.size());
      for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto &candidate = candidates[index];
        if (candidate.requiresAdjacentPartner) {
          bool hasPartner = false;
          for (std::size_t otherIndex = 0; otherIndex < candidates.size();
               ++otherIndex) {
            if (index == otherIndex)
              continue;
            const auto &other = candidates[otherIndex];
            if (sameOperand(candidate.gep.operands[0],
                            other.gep.operands[0]) &&
                candidate.gep.operands[2].intValue ==
                    other.gep.operands[2].intValue &&
                (candidate.inductionOffset + 1 == other.inductionOffset ||
                 other.inductionOffset + 1 == candidate.inductionOffset)) {
              hasPartner = true;
              break;
            }
          }
          if (!hasPartner)
            continue;
        }
        pairedCandidates.push_back(candidate);
      }
      candidates = std::move(pairedCandidates);
      if (candidates.empty()) {
        continue;
      }
      std::sort(candidates.begin(), candidates.end(),
                [](const Candidate &lhs, const Candidate &rhs) {
                  if (lhs.estimatedSaving != rhs.estimatedSaving)
                    return lhs.estimatedSaving > rhs.estimatedSaving;
                  return lhs.inductionOffset < rhs.inductionOffset;
                });
      const std::size_t maximumCandidates =
          (interleavedMultiplySubReduction ||
           fourWaySelectLoop)
              ? 4
              : 2;
      if (candidates.size() > maximumCandidates) {
        candidates.resize(maximumCandidates);
      }
      return Plan{loop, *induction, std::move(candidates)};
    }
  }
  return std::nullopt;
}

std::unordered_set<std::string>
collectVirtualNames(const MachineFunction &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters) {
    names.insert(parameter.name);
  }
  for (const auto &block : function.blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.hasResult && isVirtualRegister(instruction.result)) {
        names.insert(instruction.result.text);
      }
    }
  }
  return names;
}

MachineOperand freshVirtual(std::unordered_set<std::string> &names,
                            const std::string &stem,
                            const compiler::backend::mir::ValueType &type) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto name = stem + "." + std::to_string(suffix);
    if (names.insert(name).second) {
      return MachineOperand::virtualRegister(name, type);
    }
  }
}

void removeFullyReplacedInduction(MachineFunction &function,
                                  const Plan &plan) {
  if (plan.induction.step != 2 || !isVirtualRegister(plan.induction.phi))
    return;

  std::unordered_set<std::string> recurrenceValues{plan.induction.phi.text};
  for (const auto &instruction : function.blocks[plan.loop.header].instructions) {
    if (instruction.kind != MachineInstructionKind::Phi ||
        !instruction.hasResult ||
        instruction.result.text != plan.induction.phi.text) {
      continue;
    }
    for (const auto &incoming : instruction.phiInputs) {
      if (isVirtualRegister(incoming.value) &&
          incoming.block != function.blocks[plan.loop.preheader].name) {
        recurrenceValues.insert(incoming.value.text);
      }
    }
    break;
  }
  for (const auto &candidate : plan.candidates) {
    if (isVirtualRegister(candidate.gep.operands[1]))
      recurrenceValues.insert(candidate.gep.operands[1].text);
  }

  for (const auto &block : function.blocks) {
    for (const auto &instruction : block.instructions) {
      bool usesRecurrence = false;
      for (const auto &operand : instruction.operands) {
        if (isVirtualRegister(operand) &&
            recurrenceValues.find(operand.text) != recurrenceValues.end()) {
          usesRecurrence = true;
          break;
        }
      }
      if (!usesRecurrence && instruction.kind == MachineInstructionKind::Phi) {
        for (const auto &incoming : instruction.phiInputs) {
          if (isVirtualRegister(incoming.value) &&
              recurrenceValues.find(incoming.value.text) !=
                  recurrenceValues.end()) {
            usesRecurrence = true;
            break;
          }
        }
      }
      if (usesRecurrence &&
          (!instruction.hasResult || !isVirtualRegister(instruction.result) ||
           recurrenceValues.find(instruction.result.text) ==
               recurrenceValues.end())) {
        return;
      }
    }
  }

  for (auto &block : function.blocks) {
    block.instructions.erase(
        std::remove_if(block.instructions.begin(), block.instructions.end(),
                       [&](const MachineInstruction &instruction) {
                         return instruction.hasResult &&
                                isVirtualRegister(instruction.result) &&
                                recurrenceValues.find(instruction.result.text) !=
                                    recurrenceValues.end();
                       }),
        block.instructions.end());
  }
}

std::size_t insertionPoint(const MachineBlock &block) {
  std::size_t index = block.instructions.size();
  while (index > 0 && (block.instructions[index - 1].opcode == "br" ||
                       block.instructions[index - 1].opcode == "ret")) {
    --index;
  }
  return index;
}

bool isStrictSubset(const std::unordered_set<std::size_t> &lhs,
                    const std::unordered_set<std::size_t> &rhs) {
  if (lhs.size() >= rhs.size())
    return false;
  return std::all_of(lhs.begin(), lhs.end(), [&](const auto block) {
    return rhs.find(block) != rhs.end();
  });
}

void hoistInnermostBranchConstants(
    MachineFunction &function,
    const compiler::backend::target::TargetInstrAdapter &adapter) {
  const bool functionHasCall = std::any_of(
      function.blocks.begin(), function.blocks.end(), [](const auto &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [](const auto &instruction) {
                             return instruction.opcode == "call";
                           });
      });
  if (!functionHasCall)
    return;

  const auto cfg = CFGBuilder{}.build(function, adapter);
  const auto reachable = computeReachable(cfg);
  const auto dominators = computeDominators(cfg, reachable);
  const auto loops = findLoops(cfg, reachable, dominators);
  auto names = collectVirtualNames(function);

  for (const auto &loop : loops) {
    const bool containsNestedLoop =
        std::any_of(loops.begin(), loops.end(), [&](const auto &candidate) {
          return isStrictSubset(candidate.blocks, loop.blocks);
        });
    if (containsNestedLoop)
      continue;

    bool hasCall = false;
    std::unordered_map<long long, std::size_t> frequencies;
    std::unordered_map<long long, compiler::backend::mir::ValueType> types;
    for (const auto blockIndex : loop.blocks) {
      for (const auto &instruction : function.blocks[blockIndex].instructions) {
        if (instruction.opcode == "call")
          hasCall = true;
        const bool isCompare = instruction.opcode.rfind("icmp.", 0) == 0 ||
                               instruction.opcode.rfind("br.icmp.", 0) == 0;
        if (!isCompare || instruction.operands.size() < 2) {
          continue;
        }
        for (std::size_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
          const auto &operand = instruction.operands[operandIndex];
          if (operand.kind != MachineOperandKind::ImmediateInt ||
              !operand.type.isInt32() || operand.intValue == 0) {
            continue;
          }
          ++frequencies[operand.intValue];
          types.emplace(operand.intValue, operand.type);
        }
      }
    }
    if (hasCall || frequencies.empty())
      continue;

    const auto best = std::max_element(frequencies.begin(), frequencies.end(),
                                       [](const auto &lhs, const auto &rhs) {
                                         if (lhs.second != rhs.second)
                                           return lhs.second < rhs.second;
                                         return lhs.first < rhs.first;
                                       });
    const auto constant =
        freshVirtual(names, "%loop.branch.constant", types.at(best->first));
    auto &preheader = function.blocks[loop.preheader];
    const auto position = insertionPoint(preheader);
    preheader.instructions.insert(
        preheader.instructions.begin() + position,
        MachineInstruction::target("copy", constant,
                                   {MachineOperand::immediateInt(
                                       best->first, types.at(best->first))}));

    for (const auto blockIndex : loop.blocks) {
      for (auto &instruction : function.blocks[blockIndex].instructions) {
        const bool isCompare = instruction.opcode.rfind("icmp.", 0) == 0 ||
                               instruction.opcode.rfind("br.icmp.", 0) == 0;
        if (!isCompare || instruction.operands.size() < 2) {
          continue;
        }
        for (std::size_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
          auto &operand = instruction.operands[operandIndex];
          if (operand.kind == MachineOperandKind::ImmediateInt &&
              operand.type.isInt32() && operand.intValue == best->first) {
            operand = constant;
          }
        }
      }
    }
  }
}

bool transform(MachineFunction &function, const Plan &plan) {
  auto names = collectVirtualNames(function);
  std::vector<MachineInstruction> preheaderInitializers;
  std::vector<MachineInstruction> headerPhis;
  std::unordered_map<std::size_t, std::vector<MachineInstruction>> latchUpdates;
  std::unordered_map<std::string, MachineOperand> replacements;
  std::unordered_map<std::size_t, std::unordered_set<std::size_t>> removals;

  std::vector<std::optional<std::size_t>> sharedRoot(plan.candidates.size());
  if (plan.induction.step == 2) {
    for (std::size_t root = 0; root < plan.candidates.size(); ++root) {
      for (std::size_t adjacent = 0; adjacent < plan.candidates.size();
           ++adjacent) {
        if (root == adjacent || sharedRoot[adjacent].has_value())
          continue;
        if (sameOperand(plan.candidates[root].gep.operands[0],
                        plan.candidates[adjacent].gep.operands[0]) &&
            plan.candidates[root].gep.operands[2].intValue ==
                plan.candidates[adjacent].gep.operands[2].intValue &&
            plan.candidates[adjacent].inductionOffset ==
                plan.candidates[root].inductionOffset + 1) {
          sharedRoot[adjacent] = root;
          break;
        }
      }
    }
  }
  std::unordered_map<std::size_t, MachineOperand> sharedPointers;

  for (std::size_t candidateIndex = 0; candidateIndex < plan.candidates.size();
       ++candidateIndex) {
    const auto &candidate = plan.candidates[candidateIndex];
    if (sharedRoot[candidateIndex].has_value()) {
      const auto pointer = sharedPointers.find(*sharedRoot[candidateIndex]);
      if (pointer == sharedPointers.end())
        return false;
      function.blocks[candidate.block].instructions[candidate.instruction] =
          MachineInstruction::target(
              candidate.gep.opcode, candidate.gep.result,
              {pointer->second,
               MachineOperand::immediateInt(
                   1, compiler::backend::mir::ValueType::int32Type()),
               candidate.gep.operands[2]});
      continue;
    }
    auto initialIndex = plan.induction.initial;
    if (candidate.inductionOffset != 0) {
      initialIndex =
          freshVirtual(names, "%loop.pointer.index.initial",
                       compiler::backend::mir::ValueType::int32Type());
      preheaderInitializers.push_back(MachineInstruction::target(
          "add", initialIndex,
          {plan.induction.initial,
           MachineOperand::immediateInt(
               candidate.inductionOffset,
               compiler::backend::mir::ValueType::int32Type())}));
    }
    const auto initialPointer =
        freshVirtual(names, "%loop.pointer.initial", candidate.gep.result.type);
    preheaderInitializers.push_back(MachineInstruction::target(
        candidate.gep.opcode, initialPointer,
        {candidate.gep.operands[0], initialIndex, candidate.gep.operands[2]}));

    const auto pointerPhi =
        freshVirtual(names, "%loop.pointer", candidate.gep.result.type);
    if (std::find(sharedRoot.begin(), sharedRoot.end(), candidateIndex) !=
        sharedRoot.end()) {
      sharedPointers.emplace(candidateIndex, pointerPhi);
    }
    std::optional<MachineOperand> hoistedStride;
    if (candidate.hoistStride) {
      hoistedStride = freshVirtual(names, "%loop.pointer.stride",
                                   candidate.gep.result.type);
      preheaderInitializers.push_back(MachineInstruction::copy(
          *hoistedStride,
          MachineOperand::immediateInt(
              static_cast<long long>(
                  static_cast<__int128>(candidate.gep.operands[2].intValue) *
                  plan.induction.step),
              candidate.gep.result.type)));
    }
    std::vector<compiler::backend::mir::MachinePhiIncoming> incoming;
    incoming.push_back(
        {function.blocks[plan.loop.preheader].name, initialPointer});
    for (const auto latch : plan.loop.latches) {
      const auto nextPointer =
          freshVirtual(names, "%loop.pointer.next", candidate.gep.result.type);
      if (hoistedStride.has_value()) {
        latchUpdates[latch].push_back(MachineInstruction::target(
            "add", nextPointer, {pointerPhi, *hoistedStride}));
      } else {
        latchUpdates[latch].push_back(MachineInstruction::target(
            candidate.gep.opcode, nextPointer,
            {pointerPhi,
             MachineOperand::immediateInt(
                 plan.induction.step,
                 compiler::backend::mir::ValueType::int32Type()),
             candidate.gep.operands[2]}));
      }
      incoming.push_back({function.blocks[latch].name, nextPointer});
    }
    headerPhis.push_back(
        MachineInstruction::phi(pointerPhi, std::move(incoming)));
    replacements.emplace(candidate.gep.result.text, pointerPhi);
    removals[candidate.block].insert(candidate.instruction);
  }

  auto &preheader = function.blocks[plan.loop.preheader];
  const auto preheaderPosition = insertionPoint(preheader);
  preheader.instructions.insert(
      preheader.instructions.begin() + preheaderPosition,
      preheaderInitializers.begin(), preheaderInitializers.end());

  auto &header = function.blocks[plan.loop.header].instructions;
  header.insert(header.begin(), headerPhis.begin(), headerPhis.end());
  for (auto &[latch, updates] : latchUpdates) {
    auto &block = function.blocks[latch];
    const auto position = insertionPoint(block);
    block.instructions.insert(block.instructions.begin() + position,
                              updates.begin(), updates.end());
  }

  for (std::size_t block = 0; block < function.blocks.size(); ++block) {
    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(function.blocks[block].instructions.size());
    for (std::size_t instruction = 0;
         instruction < function.blocks[block].instructions.size();
         ++instruction) {
      std::optional<std::size_t> originalInstruction = instruction;
      if (block == plan.loop.header) {
        if (instruction < headerPhis.size()) {
          originalInstruction.reset();
        } else {
          originalInstruction = instruction - headerPhis.size();
        }
      }
      if (originalInstruction.has_value() &&
          removals[block].find(*originalInstruction) != removals[block].end()) {
        continue;
      }
      auto current = function.blocks[block].instructions[instruction];
      for (auto &operand : current.operands) {
        if (isVirtualRegister(operand)) {
          const auto replacement = replacements.find(operand.text);
          if (replacement != replacements.end()) {
            operand = replacement->second;
          }
        }
      }
      for (auto &incoming : current.phiInputs) {
        if (isVirtualRegister(incoming.value)) {
          const auto replacement = replacements.find(incoming.value.text);
          if (replacement != replacements.end()) {
            incoming.value = replacement->second;
          }
        }
      }
      rewritten.push_back(std::move(current));
    }
    function.blocks[block].instructions = std::move(rewritten);
  }
  removeFullyReplacedInduction(function, plan);
  return !replacements.empty();
}

} // namespace

void MachineLoopStrengthReduction::run(
    compiler::backend::mir::MachineModule &module) const {
  const auto &adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  for (auto &function : module.functions) {
    std::size_t instructionCount = 0;
    for (const auto &block : function.blocks) {
      instructionCount += block.instructions.size();
    }
    if (instructionCount > kMaxAnalyzedInstructions) {
      continue;
    }
    std::unordered_set<std::string> transformedHeaders;
    while (const auto plan = findPlan(function, adapter, transformedHeaders,
                                      module.target.name() == "aarch64")) {
      transformedHeaders.insert(function.blocks[plan->loop.header].name);
      if (!transform(function, *plan)) {
        break;
      }
    }
    hoistInnermostBranchConstants(function, adapter);
  }
}

} // namespace compiler::backend::opt
