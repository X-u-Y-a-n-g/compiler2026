#include "../../../include/compiler/backend/regalloc/RegisterAllocator.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::regalloc {

namespace {

using compiler::backend::common::CFGBuilder;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::mir::StackObject;
using compiler::backend::mir::StackObjectKind;
using compiler::backend::mir::ValueType;

struct Segment {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct Interval {
  std::string name;
  ValueType type;
  std::vector<Segment> segments;
  bool crossesCall = false;

  void addSegment(std::size_t start, std::size_t end) {
    if (start >= end) {
      return;
    }
    segments.push_back(Segment{start, end});
  }

  void merge() {
    if (segments.empty()) {
      return;
    }
    std::sort(segments.begin(), segments.end(), [](const Segment& lhs, const Segment& rhs) {
      if (lhs.start != rhs.start) {
        return lhs.start < rhs.start;
      }
      return lhs.end < rhs.end;
    });

    std::vector<Segment> merged;
    merged.push_back(segments.front());
    for (std::size_t i = 1; i < segments.size(); ++i) {
      if (segments[i].start <= merged.back().end) {
        merged.back().end = std::max(merged.back().end, segments[i].end);
      } else {
        merged.push_back(segments[i]);
      }
    }
    segments.swap(merged);
  }

  [[nodiscard]] std::size_t begin() const {
    return segments.empty() ? 0 : segments.front().start;
  }

  [[nodiscard]] std::size_t end() const {
    return segments.empty() ? 0 : segments.back().end;
  }
};

struct ActiveInterval {
  std::size_t end = 0;
  std::string name;
  std::string reg;
};

struct AllocationClass {
  std::vector<std::string> allocatable;
  std::unordered_set<std::string> calleeSaved;
};

bool isVirtualRegister(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister;
}

bool isRegisterCandidate(const ValueType& type) {
  return !type.isVoid();
}

int ensureSlot(MachineFunction& function, std::unordered_map<std::string, int>& slotMap,
               const std::string& name, const ValueType& type,
               const compiler::backend::target::TargetInfo& target) {
  const auto found = slotMap.find(name);
  if (found != slotMap.end()) {
    return found->second;
  }

  StackObject object;
  object.kind = StackObjectKind::Spill;
  object.name = name;
  object.size = type.size(target);
  object.alignment = type.alignment(target);
  function.stackObjects.push_back(object);

  const int slot = static_cast<int>(function.stackObjects.size() - 1);
  slotMap.emplace(name, slot);
  return slot;
}

void collectBlockUsesAndDefs(
    const compiler::backend::target::TargetInstrAdapter& adapter, const MachineFunction& function,
    const std::unordered_set<std::string>& parameterNames, std::unordered_map<std::string, ValueType>& regTypes,
    std::vector<std::unordered_set<std::string>>& useSets,
    std::vector<std::unordered_set<std::string>>& defSets) {
  useSets.assign(function.blocks.size(), {});
  defSets.assign(function.blocks.size(), {});

  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
    std::unordered_set<std::string> seenDefs;
    for (const auto& instruction : function.blocks[blockIndex].instructions) {
      std::vector<MachineOperand> uses;
      std::vector<MachineOperand> defs;
      adapter.enumerateUses(instruction, uses);
      adapter.enumerateDefs(instruction, defs);

      for (const auto& operand : uses) {
        if (!isVirtualRegister(operand) || parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        regTypes.emplace(operand.text, operand.type);
        if (seenDefs.find(operand.text) == seenDefs.end()) {
          useSets[blockIndex].insert(operand.text);
        }
      }

      for (const auto& operand : defs) {
        if (!isVirtualRegister(operand) || parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        regTypes.emplace(operand.text, operand.type);
        defSets[blockIndex].insert(operand.text);
        seenDefs.insert(operand.text);
      }
    }
  }
}

std::vector<Interval> buildIntervals(
    const compiler::backend::target::TargetInstrAdapter& adapter, const MachineFunction& function,
    const std::unordered_set<std::string>& parameterNames,
    const std::unordered_map<std::string, ValueType>& regTypes,
    const std::vector<std::unordered_set<std::string>>& liveOut,
    const std::vector<std::pair<std::size_t, std::size_t>>& blockRanges,
    const std::unordered_set<std::size_t>& callPoints) {
  std::unordered_map<std::string, Interval> intervals;
  for (const auto& [name, type] : regTypes) {
    intervals.emplace(name, Interval{name, type, {}, false});
  }

  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
    const auto [blockStart, blockEnd] = blockRanges[blockIndex];
    std::unordered_map<std::string, std::size_t> liveEnds;
    for (const auto& reg : liveOut[blockIndex]) {
      if (parameterNames.find(reg) == parameterNames.end()) {
        liveEnds.emplace(reg, blockEnd);
      }
    }

    std::size_t position = blockEnd;
    for (auto it = function.blocks[blockIndex].instructions.rbegin();
         it != function.blocks[blockIndex].instructions.rend(); ++it) {
      --position;
      std::vector<MachineOperand> uses;
      std::vector<MachineOperand> defs;
      adapter.enumerateUses(*it, uses);
      adapter.enumerateDefs(*it, defs);

      for (const auto& operand : defs) {
        if (!isVirtualRegister(operand) || parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        auto& interval = intervals.at(operand.text);
        const auto live = liveEnds.find(operand.text);
        if (live != liveEnds.end()) {
          interval.addSegment(position, live->second);
          liveEnds.erase(live);
        } else {
          interval.addSegment(position, position + 1);
        }
      }

      for (const auto& operand : uses) {
        if (!isVirtualRegister(operand) || parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        if (liveEnds.find(operand.text) == liveEnds.end()) {
          liveEnds.emplace(operand.text, position + 1);
        }
      }
    }

    for (const auto& [name, segmentEnd] : liveEnds) {
      intervals.at(name).addSegment(blockStart, segmentEnd);
    }
  }

  std::vector<Interval> result;
  result.reserve(intervals.size());
  for (auto& [_, interval] : intervals) {
    interval.merge();
    for (const auto& segment : interval.segments) {
      for (const auto callPoint : callPoints) {
        if (segment.start <= callPoint && callPoint < segment.end) {
          interval.crossesCall = true;
          break;
        }
      }
      if (interval.crossesCall) {
        break;
      }
    }
    result.push_back(std::move(interval));
  }

  std::sort(result.begin(), result.end(), [](const Interval& lhs, const Interval& rhs) {
    if (lhs.begin() != rhs.begin()) {
      return lhs.begin() < rhs.begin();
    }
    return lhs.end() < rhs.end();
  });
  return result;
}

void expireOldIntervals(std::vector<ActiveInterval>& active, std::vector<std::string>& freeRegs,
                        std::size_t position) {
  std::vector<ActiveInterval> stillActive;
  stillActive.reserve(active.size());
  for (auto& entry : active) {
    if (entry.end <= position) {
      freeRegs.push_back(entry.reg);
    } else {
      stillActive.push_back(std::move(entry));
    }
  }
  active.swap(stillActive);
}

std::vector<std::string> orderedFreeRegisters(const std::vector<std::string>& freeRegs,
                                              const std::unordered_set<std::string>& calleeSaved,
                                              bool preferCalleeSaved) {
  std::vector<std::string> ordered = freeRegs;
  std::sort(ordered.begin(), ordered.end(), [&](const std::string& lhs, const std::string& rhs) {
    const bool lhsSaved = calleeSaved.find(lhs) != calleeSaved.end();
    const bool rhsSaved = calleeSaved.find(rhs) != calleeSaved.end();
    if (preferCalleeSaved && lhsSaved != rhsSaved) {
      return lhsSaved > rhsSaved;
    }
    if (!preferCalleeSaved && lhsSaved != rhsSaved) {
      return lhsSaved < rhsSaved;
    }
    return lhs < rhs;
  });
  return ordered;
}

void allocateClassIntervals(const std::vector<Interval>& intervals, const AllocationClass& klass,
                            bool selectFloat,
                            std::unordered_map<std::string, std::string>& registerMap) {
  std::vector<ActiveInterval> active;
  std::vector<std::string> freeRegs = klass.allocatable;

  for (const auto& interval : intervals) {
    if (interval.segments.empty() || interval.type.isFloat() != selectFloat ||
        !isRegisterCandidate(interval.type)) {
      continue;
    }

    expireOldIntervals(active, freeRegs, interval.begin());
    const auto preferredFree =
        orderedFreeRegisters(freeRegs, klass.calleeSaved, interval.crossesCall);

    if (!preferredFree.empty()) {
      const std::string reg = preferredFree.front();
      freeRegs.erase(std::find(freeRegs.begin(), freeRegs.end(), reg));
      registerMap.emplace(interval.name, reg);
      active.push_back(ActiveInterval{interval.end(), interval.name, reg});
      continue;
    }

    const auto spillIt =
        std::max_element(active.begin(), active.end(), [](const ActiveInterval& lhs,
                                                          const ActiveInterval& rhs) {
          return lhs.end < rhs.end;
        });
    if (spillIt != active.end() && spillIt->end > interval.end()) {
      registerMap.erase(spillIt->name);
      registerMap.emplace(interval.name, spillIt->reg);
      *spillIt = ActiveInterval{interval.end(), interval.name, spillIt->reg};
    }
  }
}

bool keepUseInFrame(const MachineInstruction& instruction) {
  return instruction.opcode == "call" || instruction.opcode == "ret" || instruction.opcode == "br" ||
         instruction.kind == MachineInstructionKind::Phi;
}

std::string takeScratchRegister(const ValueType& type,
                                const compiler::backend::target::TargetRegInfo& regInfo,
                                std::size_t& integerIndex, std::size_t& floatIndex) {
  if (type.isFloat()) {
    if (floatIndex >= regInfo.scratchFloatRegisters().size()) {
      return {};
    }
    return regInfo.scratchFloatRegisters()[floatIndex++];
  }
  if (integerIndex >= regInfo.scratchIntegerRegisters().size()) {
    return {};
  }
  return regInfo.scratchIntegerRegisters()[integerIndex++];
}

void rewriteFunctionInstructions(
    MachineFunction& function, std::unordered_map<std::string, int>& slotMap,
    const std::unordered_map<std::string, std::string>& registerMap,
    const compiler::backend::target::TargetInfo& target,
    const compiler::backend::target::TargetRegInfo& regInfo) {
  for (auto& block : function.blocks) {
    std::vector<MachineInstruction> rewrittenBlock;

    for (const auto& original : block.instructions) {
      MachineInstruction rewritten = original;
      std::vector<MachineInstruction> before;
      std::vector<MachineInstruction> after;
      std::size_t nextIntegerScratch = 0;
      std::size_t nextFloatScratch = 0;

      const auto rewriteUse = [&](MachineOperand& operand) {
        if (operand.kind != MachineOperandKind::VirtualRegister) {
          return;
        }

        if (const auto reg = registerMap.find(operand.text); reg != registerMap.end()) {
          operand = MachineOperand::physicalRegister(reg->second, operand.type);
          return;
        }

        const int slot = ensureSlot(function, slotMap, operand.text, operand.type, target);
        if (keepUseInFrame(rewritten)) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        const std::string scratch =
            takeScratchRegister(operand.type, regInfo, nextIntegerScratch, nextFloatScratch);
        if (scratch.empty()) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        auto physical = MachineOperand::physicalRegister(scratch, operand.type);
        before.push_back(
            MachineInstruction::spillReload(physical, MachineOperand::stackSlot(slot, operand.type)));
        operand = std::move(physical);
      };

      const auto rewriteDef = [&](MachineOperand& operand) {
        if (operand.kind != MachineOperandKind::VirtualRegister) {
          return;
        }

        if (const auto reg = registerMap.find(operand.text); reg != registerMap.end()) {
          operand = MachineOperand::physicalRegister(reg->second, operand.type);
          return;
        }

        const int slot = ensureSlot(function, slotMap, operand.text, operand.type, target);
        if (rewritten.kind == MachineInstructionKind::Copy ||
            rewritten.kind == MachineInstructionKind::Phi) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        const std::string scratch =
            takeScratchRegister(operand.type, regInfo, nextIntegerScratch, nextFloatScratch);
        if (scratch.empty()) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        auto physical = MachineOperand::physicalRegister(scratch, operand.type);
        operand = physical;
        after.push_back(
            MachineInstruction::spillStore(std::move(physical), MachineOperand::stackSlot(slot, operand.type)));
      };

      if (rewritten.kind == MachineInstructionKind::Phi) {
        for (auto& incoming : rewritten.phiInputs) {
          rewriteUse(incoming.value);
        }
      } else {
        for (auto& operand : rewritten.operands) {
          rewriteUse(operand);
        }
      }

      if (rewritten.hasResult) {
        rewriteDef(rewritten.result);
      }

      rewrittenBlock.insert(rewrittenBlock.end(), before.begin(), before.end());
      rewrittenBlock.push_back(std::move(rewritten));
      rewrittenBlock.insert(rewrittenBlock.end(), after.begin(), after.end());
    }

    block.instructions = std::move(rewrittenBlock);
  }
}

}  // namespace

void RegisterAllocator::allocate(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);
  const auto& regInfo = compiler::backend::target::regInfoFor(module.target);

  for (auto& function : module.functions) {
    std::unordered_map<std::string, int> slotMap;
    std::unordered_set<std::string> parameterNames;
    for (auto& parameter : function.parameters) {
      parameter.homeSlot = ensureSlot(function, slotMap, parameter.name, parameter.type, module.target);
      parameterNames.insert(parameter.name);
    }

    std::vector<std::pair<std::size_t, std::size_t>> blockRanges(function.blocks.size());
    std::unordered_set<std::size_t> callPoints;
    std::size_t instructionIndex = 0;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
      const std::size_t blockStart = instructionIndex;
      for (const auto& instruction : function.blocks[blockIndex].instructions) {
        if (adapter.isCall(instruction)) {
          callPoints.insert(instructionIndex);
        }
        ++instructionIndex;
      }
      blockRanges[blockIndex] = {blockStart, instructionIndex};
    }

    std::unordered_map<std::string, ValueType> regTypes;
    std::vector<std::unordered_set<std::string>> useSets;
    std::vector<std::unordered_set<std::string>> defSets;
    collectBlockUsesAndDefs(adapter, function, parameterNames, regTypes, useSets, defSets);

    const auto cfg = CFGBuilder{}.build(function, adapter);
    std::vector<std::unordered_set<std::string>> liveIn(function.blocks.size());
    std::vector<std::unordered_set<std::string>> liveOut(function.blocks.size());
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t reverse = function.blocks.size(); reverse > 0; --reverse) {
        const std::size_t blockIndex = reverse - 1;

        std::unordered_set<std::string> newOut;
        for (const auto succ : cfg.successors[blockIndex]) {
          newOut.insert(liveIn[succ].begin(), liveIn[succ].end());
        }

        std::unordered_set<std::string> newIn = useSets[blockIndex];
        for (const auto& reg : newOut) {
          if (defSets[blockIndex].find(reg) == defSets[blockIndex].end()) {
            newIn.insert(reg);
          }
        }

        if (newOut != liveOut[blockIndex] || newIn != liveIn[blockIndex]) {
          liveOut[blockIndex] = std::move(newOut);
          liveIn[blockIndex] = std::move(newIn);
          changed = true;
        }
      }
    }

    const auto intervals =
        buildIntervals(adapter, function, parameterNames, regTypes, liveOut, blockRanges, callPoints);

    std::unordered_map<std::string, std::string> registerMap;
    allocateClassIntervals(
        intervals,
        AllocationClass{regInfo.allocatableIntegerRegisters(),
                        std::unordered_set<std::string>(regInfo.calleeSavedIntegerRegisters().begin(),
                                                        regInfo.calleeSavedIntegerRegisters().end())},
        false, registerMap);
    allocateClassIntervals(
        intervals,
        AllocationClass{regInfo.allocatableFloatRegisters(),
                        std::unordered_set<std::string>(regInfo.calleeSavedFloatRegisters().begin(),
                                                        regInfo.calleeSavedFloatRegisters().end())},
        true, registerMap);

    std::unordered_set<std::string> usedRegs;
    std::unordered_set<std::string> usedFloatRegs;
    for (const auto& [_, reg] : registerMap) {
      if (regInfo.calleeSavedIntegerRegisters().end() !=
          std::find(regInfo.calleeSavedIntegerRegisters().begin(),
                    regInfo.calleeSavedIntegerRegisters().end(), reg)) {
        usedRegs.insert(reg);
      }
      if (regInfo.calleeSavedFloatRegisters().end() !=
          std::find(regInfo.calleeSavedFloatRegisters().begin(),
                    regInfo.calleeSavedFloatRegisters().end(), reg)) {
        usedFloatRegs.insert(reg);
      }
    }
    function.usedIntegerCalleeSavedRegisters.assign(usedRegs.begin(), usedRegs.end());
    std::sort(function.usedIntegerCalleeSavedRegisters.begin(),
              function.usedIntegerCalleeSavedRegisters.end());
    function.usedFloatCalleeSavedRegisters.assign(usedFloatRegs.begin(), usedFloatRegs.end());
    std::sort(function.usedFloatCalleeSavedRegisters.begin(),
              function.usedFloatCalleeSavedRegisters.end());

    rewriteFunctionInstructions(function, slotMap, registerMap, module.target, regInfo);
  }
}

}  // namespace compiler::backend::regalloc
