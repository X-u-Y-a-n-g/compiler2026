#include "../../../include/compiler/backend/opt/AddressReassociation.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
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

using compiler::backend::common::CFGBuilder;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

struct LoopInfo {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::unordered_set<std::size_t> blocks;
};

struct InstructionLocation {
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Transformation {
  InstructionLocation first;
  InstructionLocation second;
  std::size_t preheader = 0;
  std::size_t loopHeader = 0;
  MachineOperand columnBase;
  MachineOperand rowIndex;
  MachineOperand rowScale;
  MachineInstruction preheaderInstruction;
  bool insertsPreheader = true;
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
         instruction.hasResult && instruction.operands.size() == 3;
}

std::vector<bool>
computeReachable(const compiler::backend::common::ControlFlowGraph &cfg) {
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
    for (const auto succ : cfg.successors[block]) {
      if (!reachable[succ]) {
        reachable[succ] = true;
        worklist.push_back(succ);
      }
    }
  }
  return reachable;
}

std::vector<std::unordered_set<std::size_t>>
computeDominators(const compiler::backend::common::ControlFlowGraph &cfg,
                  const std::vector<bool> &reachable) {
  const std::size_t count = cfg.blocks.size();
  std::unordered_set<std::size_t> allReachable;
  for (std::size_t block = 0; block < count; ++block) {
    if (reachable[block]) {
      allReachable.insert(block);
    }
  }

  std::vector<std::unordered_set<std::size_t>> dominators(count);
  if (count == 0) {
    return dominators;
  }
  dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    if (reachable[block]) {
      dominators[block] = allReachable;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (!reachable[block]) {
        continue;
      }
      std::optional<std::unordered_set<std::size_t>> intersection;
      for (const auto pred : cfg.predecessors[block]) {
        if (!reachable[pred]) {
          continue;
        }
        if (!intersection.has_value()) {
          intersection = dominators[pred];
          continue;
        }
        std::unordered_set<std::size_t> next;
        for (const auto candidate : *intersection) {
          if (dominators[pred].find(candidate) != dominators[pred].end()) {
            next.insert(candidate);
          }
        }
        intersection = std::move(next);
      }
      std::unordered_set<std::size_t> next =
          intersection.has_value() ? std::move(*intersection)
                                   : std::unordered_set<std::size_t>{};
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
findLoops(const compiler::backend::common::ControlFlowGraph &cfg,
          const std::vector<bool> &reachable,
          const std::vector<std::unordered_set<std::size_t>> &dominators) {
  std::vector<LoopInfo> loops;
  for (std::size_t latch = 0; latch < cfg.blocks.size(); ++latch) {
    if (!reachable[latch]) {
      continue;
    }
    for (const auto header : cfg.successors[latch]) {
      if (dominators[latch].find(header) == dominators[latch].end()) {
        continue;
      }
      LoopInfo loop;
      loop.header = header;
      loop.blocks.insert(header);
      loop.blocks.insert(latch);
      std::deque<std::size_t> worklist;
      worklist.push_back(latch);
      while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop_front();
        for (const auto pred : cfg.predecessors[block]) {
          if (loop.blocks.insert(pred).second) {
            worklist.push_back(pred);
          }
        }
      }

      std::optional<std::size_t> preheader;
      for (const auto pred : cfg.predecessors[header]) {
        if (loop.blocks.find(pred) != loop.blocks.end()) {
          continue;
        }
        if (preheader.has_value()) {
          preheader.reset();
          break;
        }
        preheader = pred;
      }
      if (preheader.has_value()) {
        loop.preheader = *preheader;
        loops.push_back(std::move(loop));
      }
    }
  }
  std::sort(loops.begin(), loops.end(),
            [](const LoopInfo &lhs, const LoopInfo &rhs) {
              return lhs.blocks.size() < rhs.blocks.size();
            });
  return loops;
}

MachineOperand
resolveAlias(MachineOperand operand,
             const std::unordered_map<std::string, MachineOperand> &aliases) {
  std::unordered_set<std::string> seen;
  while (isVirtualRegister(operand) && seen.insert(operand.text).second) {
    const auto found = aliases.find(operand.text);
    if (found == aliases.end()) {
      break;
    }
    operand = found->second;
  }
  return operand;
}

bool hasDefinitionInside(
    const MachineOperand &operand, const LoopInfo &loop,
    const std::unordered_map<std::string, std::unordered_set<std::size_t>>
        &definitionBlocks) {
  if (!isVirtualRegister(operand)) {
    return false;
  }
  const auto found = definitionBlocks.find(operand.text);
  if (found == definitionBlocks.end()) {
    return false;
  }
  for (const auto block : found->second) {
    if (loop.blocks.find(block) != loop.blocks.end()) {
      return true;
    }
  }
  return false;
}

void insertBeforeTerminator(
    compiler::backend::mir::MachineBlock &block, MachineInstruction instruction,
    const compiler::backend::target::TargetInstrAdapter &adapter) {
  auto position = block.instructions.end();
  if (!block.instructions.empty()) {
    const auto &last = block.instructions.back();
    if (adapter.isConditionalBranch(last) ||
        adapter.isUnconditionalBranch(last) || adapter.isReturn(last)) {
      position = block.instructions.end() - 1;
    }
  }
  block.instructions.insert(position, std::move(instruction));
}

std::uint64_t nestedLoopWeight(unsigned relativeDepth) {
  std::uint64_t weight = 1;
  while (relativeDepth-- > 0 && weight < 4096) {
    weight *= 8;
  }
  return weight;
}

void promoteNestedGlobalAddresses(
    MachineFunction &function, const std::vector<LoopInfo> &loops,
    const compiler::backend::target::TargetInstrAdapter &adapter,
    std::size_t &nextTemporary) {
  if (loops.empty()) {
    return;
  }

  std::vector<unsigned> blockDepth(function.blocks.size(), 0);
  for (const auto &loop : loops) {
    for (const auto block : loop.blocks) {
      ++blockDepth[block];
    }
  }

  std::vector<const LoopInfo *> outerFirst;
  outerFirst.reserve(loops.size());
  for (const auto &loop : loops) {
    outerFirst.push_back(&loop);
  }
  std::sort(outerFirst.begin(), outerFirst.end(),
            [](const LoopInfo *lhs, const LoopInfo *rhs) {
              return lhs->blocks.size() > rhs->blocks.size();
            });

  struct Candidate {
    MachineOperand address;
    std::uint64_t benefit = 0;
  };

  for (const auto *loop : outerFirst) {
    std::unordered_map<std::string, Candidate> candidates;
    const unsigned headerDepth = blockDepth[loop->header];
    for (const auto blockIndex : loop->blocks) {
      const unsigned relativeDepth =
          blockDepth[blockIndex] >= headerDepth
              ? blockDepth[blockIndex] - headerDepth + 1
              : 1;
      const std::uint64_t weight = nestedLoopWeight(relativeDepth);
      for (const auto &instruction : function.blocks[blockIndex].instructions) {
        for (const auto &operand : instruction.operands) {
          if (operand.kind != MachineOperandKind::GlobalAddress) {
            continue;
          }
          auto &candidate = candidates[operand.text];
          candidate.address = operand;
          candidate.benefit += weight;
        }
        for (const auto &incoming : instruction.phiInputs) {
          if (incoming.value.kind != MachineOperandKind::GlobalAddress) {
            continue;
          }
          auto &candidate = candidates[incoming.value.text];
          candidate.address = incoming.value;
          candidate.benefit += weight;
        }
      }
    }

    std::vector<Candidate> profitable;
    for (auto &[_, candidate] : candidates) {
      // A single address use in one loop is usually cheaper than extending a
      // live range. Nested-loop uses have exponentially larger estimated
      // savings.
      if (candidate.benefit >= 64) {
        profitable.push_back(std::move(candidate));
      }
    }
    std::sort(profitable.begin(), profitable.end(),
              [](const Candidate &lhs, const Candidate &rhs) {
                return lhs.benefit > rhs.benefit;
              });
    if (profitable.size() > 3) {
      profitable.resize(3);
    }

    for (const auto &candidate : profitable) {
      const MachineOperand promoted = MachineOperand::virtualRegister(
          "%global.address." + std::to_string(nextTemporary++),
          candidate.address.type);
      insertBeforeTerminator(
          function.blocks[loop->preheader],
          MachineInstruction::copy(promoted, candidate.address), adapter);

      for (const auto blockIndex : loop->blocks) {
        for (auto &instruction : function.blocks[blockIndex].instructions) {
          for (auto &operand : instruction.operands) {
            if (operand.kind == MachineOperandKind::GlobalAddress &&
                operand.text == candidate.address.text) {
              operand = promoted;
            }
          }
          for (auto &incoming : instruction.phiInputs) {
            if (incoming.value.kind == MachineOperandKind::GlobalAddress &&
                incoming.value.text == candidate.address.text) {
              incoming.value = promoted;
            }
          }
        }
      }
    }
  }
}

} // namespace

namespace {

constexpr std::size_t kMaxEarlyAnalysisInstructions = 4096;

void runAddressReassociation(compiler::backend::mir::MachineModule &module,
                             bool promoteGlobalAddresses) {
  const auto &adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  std::size_t nextTemporary = 0;
  for (auto &function : module.functions) {
    if (function.blocks.size() > 128) {
      continue;
    }
    if (!promoteGlobalAddresses) {
      std::size_t instructionCount = 0;
      for (const auto &block : function.blocks) {
        instructionCount += block.instructions.size();
      }
      if (instructionCount > kMaxEarlyAnalysisInstructions) {
        continue;
      }
    }
    const auto cfg = CFGBuilder{}.build(function, adapter);
    const auto reachable = computeReachable(cfg);
    const auto dominators = computeDominators(cfg, reachable);
    const auto loops = findLoops(cfg, reachable, dominators);

    std::unordered_map<std::string, std::size_t> definitionCounts;
    std::unordered_map<std::string, std::unordered_set<std::size_t>>
        definitionBlocks;
    std::unordered_map<std::string, InstructionLocation> definitionLocations;
    std::unordered_map<std::string, std::size_t> useCounts;
    std::vector<MachineOperand> uses;
    std::vector<MachineOperand> defs;
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
      for (std::size_t instruction = 0;
           instruction < function.blocks[block].instructions.size();
           ++instruction) {
        const auto &current = function.blocks[block].instructions[instruction];
        adapter.enumerateDefs(current, defs);
        for (const auto &operand : defs) {
          if (!isVirtualRegister(operand)) {
            continue;
          }
          ++definitionCounts[operand.text];
          definitionBlocks[operand.text].insert(block);
          definitionLocations[operand.text] = {block, instruction};
        }
        adapter.enumerateUses(current, uses);
        for (const auto &operand : uses) {
          if (isVirtualRegister(operand)) {
            ++useCounts[operand.text];
          }
        }
      }
    }

    std::unordered_map<std::string, MachineOperand> aliases;
    for (const auto &block : function.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.opcode == "copy" && instruction.hasResult &&
            instruction.operands.size() == 1 &&
            isVirtualRegister(instruction.result) &&
            definitionCounts[instruction.result.text] == 1) {
          aliases[instruction.result.text] = instruction.operands[0];
        }
      }
    }

    std::vector<Transformation> transformations;
    std::unordered_set<std::string> claimedIntermediate;
    for (const auto &loop : loops) {
      for (const auto blockIndex : loop.blocks) {
        const auto &block = function.blocks[blockIndex];
        for (std::size_t secondIndex = 0;
             secondIndex < block.instructions.size(); ++secondIndex) {
          const auto &second = block.instructions[secondIndex];
          if (!isGep(second) || !isVirtualRegister(second.operands[0])) {
            continue;
          }
          const auto firstLocation =
              definitionLocations.find(second.operands[0].text);
          if (firstLocation == definitionLocations.end() ||
              firstLocation->second.block != blockIndex ||
              firstLocation->second.instruction >= secondIndex ||
              claimedIntermediate.find(second.operands[0].text) !=
                  claimedIntermediate.end()) {
            continue;
          }
          const auto &first =
              function.blocks[firstLocation->second.block]
                  .instructions[firstLocation->second.instruction];
          if (!isGep(first) ||
              first.operands[2].kind != MachineOperandKind::ImmediateInt ||
              second.operands[2].kind != MachineOperandKind::ImmediateInt ||
              first.operands[2].intValue <= second.operands[2].intValue ||
              useCounts[first.result.text] != 1) {
            continue;
          }

          const MachineOperand rowIndex =
              resolveAlias(first.operands[1], aliases);
          const MachineOperand columnIndex =
              resolveAlias(second.operands[1], aliases);
          const MachineOperand arrayBase =
              resolveAlias(first.operands[0], aliases);
          if (arrayBase.kind != MachineOperandKind::GlobalAddress &&
              std::getenv(
                  "COMPILER_DISABLE_INVARIANT_BASE_ADDRESS_REASSOCIATION") !=
                  nullptr) {
            continue;
          }
          if (!hasDefinitionInside(rowIndex, loop, definitionBlocks) ||
              hasDefinitionInside(columnIndex, loop, definitionBlocks) ||
              hasDefinitionInside(arrayBase, loop, definitionBlocks)) {
            continue;
          }
          if (isVirtualRegister(arrayBase)) {
            const auto baseDefinition =
                definitionLocations.find(arrayBase.text);
            if (baseDefinition != definitionLocations.end() &&
                dominators[loop.preheader].find(baseDefinition->second.block) ==
                    dominators[loop.preheader].end()) {
              continue;
            }
          }

          std::optional<MachineOperand> reusableColumnBase;
          for (const auto &transformation : transformations) {
            if (transformation.loopHeader != loop.header ||
                transformation.preheaderInstruction.operands.size() != 3) {
              continue;
            }
            const auto &operands = transformation.preheaderInstruction.operands;
            if (sameOperand(operands[0], arrayBase) &&
                sameOperand(operands[1], columnIndex) &&
                sameOperand(operands[2], second.operands[2])) {
              reusableColumnBase = transformation.columnBase;
              break;
            }
          }
          const bool insertsPreheader = !reusableColumnBase.has_value();
          MachineOperand columnBase =
              reusableColumnBase.value_or(MachineOperand::virtualRegister(
                  "%address.reassoc." + std::to_string(nextTemporary++),
                  second.result.type));
          MachineInstruction preheaderInstruction = MachineInstruction::target(
              "gep", columnBase, {arrayBase, columnIndex, second.operands[2]});
          transformations.push_back({firstLocation->second,
                                     {blockIndex, secondIndex},
                                     loop.preheader,
                                     loop.header,
                                     columnBase,
                                     rowIndex,
                                     first.operands[2],
                                     std::move(preheaderInstruction),
                                     insertsPreheader});
          claimedIntermediate.insert(first.result.text);
        }
      }
    }

    std::unordered_map<std::size_t, std::size_t> transformationsPerLoop;
    for (const auto &transformation : transformations) {
      ++transformationsPerLoop[transformation.loopHeader];
    }
    transformations.erase(
        std::remove_if(
            transformations.begin(), transformations.end(),
            [&](const auto &transformation) {
              return transformationsPerLoop[transformation.loopHeader] < 2;
            }),
        transformations.end());

    std::vector<std::vector<unsigned char>> remove(function.blocks.size());
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
      remove[block].assign(function.blocks[block].instructions.size(), 0);
    }
    for (const auto &transformation : transformations) {
      auto &second = function.blocks[transformation.second.block]
                         .instructions[transformation.second.instruction];
      second.operands = {transformation.columnBase, transformation.rowIndex,
                         transformation.rowScale};
      remove[transformation.first.block][transformation.first.instruction] = 1;
    }
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
      std::vector<MachineInstruction> kept;
      kept.reserve(function.blocks[block].instructions.size());
      for (std::size_t instruction = 0;
           instruction < function.blocks[block].instructions.size();
           ++instruction) {
        if (remove[block][instruction] == 0) {
          kept.push_back(
              std::move(function.blocks[block].instructions[instruction]));
        }
      }
      function.blocks[block].instructions = std::move(kept);
    }
    for (auto &transformation : transformations) {
      if (!transformation.insertsPreheader) {
        continue;
      }
      insertBeforeTerminator(function.blocks[transformation.preheader],
                             std::move(transformation.preheaderInstruction),
                             adapter);
    }
    if (promoteGlobalAddresses) {
      promoteNestedGlobalAddresses(function, loops, adapter, nextTemporary);
    }
  }
}

} // namespace

void AddressReassociation::runReassociation(
    compiler::backend::mir::MachineModule &module) const {
  runAddressReassociation(module, false);
}

void AddressReassociation::run(
    compiler::backend::mir::MachineModule &module) const {
  runAddressReassociation(module, true);
}

} // namespace compiler::backend::opt
