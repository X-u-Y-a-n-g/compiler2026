#include "../../../include/compiler/backend/regalloc/RegisterAllocator.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <queue>
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

struct GraphNode {
  std::string name;
  ValueType type;
  std::vector<std::size_t> affinities;
  std::vector<std::string> preferredRegisters;
  std::string precoloredRegister;
  double spillCost = 0.0;
  bool crossesCall = false;
  bool avoidArgumentRegisters = false;
};

struct InterferenceGraph {
  std::vector<GraphNode> nodes;
  std::unordered_map<std::string, std::size_t> indexByName;
  std::vector<std::uint64_t> adjacency;
  std::size_t wordsPerNode = 0;

  GraphNode& addNode(const std::string& name, const ValueType& type) {
    const auto found = indexByName.find(name);
    if (found != indexByName.end()) {
      auto& node = nodes[found->second];
      if (node.type.isVoid()) {
        node.type = type;
      }
      return node;
    }
    const std::size_t index = nodes.size();
    indexByName.emplace(name, index);
    nodes.push_back(GraphNode{name, type, {}, {}, {}, 0.0, false, false});
    return nodes.back();
  }

  [[nodiscard]] std::size_t indexOf(const std::string& name) const {
    return indexByName.at(name);
  }

  void finalizeNodes() {
    wordsPerNode = (nodes.size() + 63) / 64;
    adjacency.assign(nodes.size() * wordsPerNode, 0);
  }

  void addEdge(std::size_t lhs, std::size_t rhs) {
    if (lhs == rhs || nodes[lhs].type.isFloat() != nodes[rhs].type.isFloat()) {
      return;
    }
    adjacency[lhs * wordsPerNode + rhs / 64] |= std::uint64_t{1} << (rhs % 64);
    adjacency[rhs * wordsPerNode + lhs / 64] |= std::uint64_t{1} << (lhs % 64);
  }

  void addAffinity(std::size_t lhs, std::size_t rhs) {
    if (lhs == rhs || nodes[lhs].type.isFloat() != nodes[rhs].type.isFloat()) {
      return;
    }
    if (std::find(nodes[lhs].affinities.begin(), nodes[lhs].affinities.end(),
                  rhs) == nodes[lhs].affinities.end()) {
      nodes[lhs].affinities.push_back(rhs);
    }
    if (std::find(nodes[rhs].affinities.begin(), nodes[rhs].affinities.end(),
                  lhs) == nodes[rhs].affinities.end()) {
      nodes[rhs].affinities.push_back(lhs);
    }
  }

  template <typename Callback>
  void forEachNeighbor(std::size_t index, Callback&& callback) const {
    const std::size_t rowStart = index * wordsPerNode;
    for (std::size_t wordIndex = 0; wordIndex < wordsPerNode; ++wordIndex) {
      std::uint64_t bits = adjacency[rowStart + wordIndex];
      while (bits != 0) {
        const unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
        const std::size_t neighbor = wordIndex * 64 + bit;
        if (neighbor < nodes.size()) {
          callback(neighbor);
        }
        bits &= bits - 1;
      }
    }
  }
};

struct ColoringStackEntry {
  std::size_t node = 0;
};

struct AllocationClass {
  std::vector<std::string> allocatable;
  std::unordered_set<std::string> calleeSaved;
  std::unordered_set<std::string> argumentRegisters;
};

using BitVector = std::vector<std::uint64_t>;

void setBit(BitVector& bits, std::size_t index) {
  bits[index / 64] |= std::uint64_t{1} << (index % 64);
}

void clearBit(BitVector& bits, std::size_t index) {
  bits[index / 64] &= ~(std::uint64_t{1} << (index % 64));
}

template <typename Callback>
void forEachSetBit(const BitVector& bits, std::size_t limit,
                   Callback&& callback) {
  for (std::size_t wordIndex = 0; wordIndex < bits.size(); ++wordIndex) {
    std::uint64_t word = bits[wordIndex];
    while (word != 0) {
      const unsigned bit = static_cast<unsigned>(__builtin_ctzll(word));
      const std::size_t index = wordIndex * 64 + bit;
      if (index < limit) {
        callback(index);
      }
      word &= word - 1;
    }
  }
}

bool isVirtualRegister(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister;
}

bool isRegisterCandidate(const ValueType& type) { return !type.isVoid(); }

int ensureSlot(MachineFunction& function,
               std::unordered_map<std::string, int>& slotMap,
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
    const compiler::backend::target::TargetInstrAdapter& adapter,
    const MachineFunction& function,
    const std::unordered_set<std::string>& parameterNames,
    std::unordered_map<std::string, ValueType>& regTypes,
    std::vector<std::unordered_set<std::string>>& useSets,
    std::vector<std::unordered_set<std::string>>& defSets) {
  useSets.assign(function.blocks.size(), {});
  defSets.assign(function.blocks.size(), {});

  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    std::unordered_set<std::string> seenDefs;
    for (const auto& instruction : function.blocks[blockIndex].instructions) {
      std::vector<MachineOperand> uses;
      std::vector<MachineOperand> defs;
      adapter.enumerateUses(instruction, uses);
      adapter.enumerateDefs(instruction, defs);

      for (const auto& operand : uses) {
        if (!isVirtualRegister(operand) ||
            parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        regTypes.emplace(operand.text, operand.type);
        if (seenDefs.find(operand.text) == seenDefs.end()) {
          useSets[blockIndex].insert(operand.text);
        }
      }

      for (const auto& operand : defs) {
        if (!isVirtualRegister(operand) ||
            parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        regTypes.emplace(operand.text, operand.type);
        defSets[blockIndex].insert(operand.text);
        seenDefs.insert(operand.text);
      }
    }
  }

}

std::vector<unsigned> computeLoopDepths(
    const compiler::backend::common::ControlFlowGraph& cfg) {
  const std::size_t blockCount = cfg.blocks.size();
  std::vector<unsigned> loopDepth(blockCount, 0);
  if (blockCount == 0) {
    return loopDepth;
  }

  const std::size_t wordCount = (blockCount + 63) / 64;
  const std::uint64_t lastWordMask =
      blockCount % 64 == 0 ? ~std::uint64_t{0}
                           : (std::uint64_t{1} << (blockCount % 64)) - 1;
  std::vector<std::vector<std::uint64_t>> dominators(
      blockCount, std::vector<std::uint64_t>(wordCount, ~std::uint64_t{0}));
  dominators[0].assign(wordCount, 0);
  dominators[0][0] = 1;
  for (std::size_t block = 1; block < blockCount; ++block) {
    dominators[block].back() &= lastWordMask;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < blockCount; ++block) {
      std::vector<std::uint64_t> updated(wordCount, 0);
      if (cfg.predecessors[block].empty()) {
        // An unreachable block is only dominated by itself.
      } else {
        updated = dominators[cfg.predecessors[block].front()];
        for (std::size_t predIndex = 1;
             predIndex < cfg.predecessors[block].size(); ++predIndex) {
          const auto& predDominators =
              dominators[cfg.predecessors[block][predIndex]];
          for (std::size_t word = 0; word < wordCount; ++word) {
            updated[word] &= predDominators[word];
          }
        }
      }
      updated[block / 64] |= std::uint64_t{1} << (block % 64);
      if (updated != dominators[block]) {
        dominators[block] = std::move(updated);
        changed = true;
      }
    }
  }

  for (std::size_t tail = 0; tail < blockCount; ++tail) {
    for (const auto head : cfg.successors[tail]) {
      if ((dominators[tail][head / 64] & (std::uint64_t{1} << (head % 64))) ==
          0) {
        continue;
      }

      std::unordered_set<std::size_t> loopBlocks{head, tail};
      std::vector<std::size_t> worklist;
      if (tail != head) {
        worklist.push_back(tail);
      }
      while (!worklist.empty()) {
        const std::size_t block = worklist.back();
        worklist.pop_back();
        for (const auto pred : cfg.predecessors[block]) {
          if (loopBlocks.insert(pred).second && pred != head) {
            worklist.push_back(pred);
          }
        }
      }
      for (const auto block : loopBlocks) {
        ++loopDepth[block];
      }
    }
  }
  return loopDepth;
}

double blockWeight(unsigned loopDepth) {
  double weight = 1.0;
  while (loopDepth-- > 0 && weight < 10000.0) {
    weight *= 10.0;
  }
  return weight;
}

std::vector<MachineOperand> virtualOperands(
    const std::vector<MachineOperand>& operands,
    const std::unordered_set<std::string>& parameterNames) {
  std::vector<MachineOperand> result;
  for (const auto& operand : operands) {
    if (isVirtualRegister(operand) &&
        parameterNames.find(operand.text) == parameterNames.end()) {
      result.push_back(operand);
    }
  }
  return result;
}

void buildInterferenceGraph(
    InterferenceGraph& graph,
    const compiler::backend::target::TargetInstrAdapter& adapter,
    const MachineFunction& function,
    const compiler::backend::common::ControlFlowGraph& cfg,
    const std::unordered_set<std::string>& stackHomeParameters,
    const std::vector<BitVector>& liveOut) {
  const auto loopDepths = computeLoopDepths(cfg);
  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    const double weight = blockWeight(loopDepths[blockIndex]);
    BitVector live = liveOut[blockIndex];

    for (auto instructionIt = function.blocks[blockIndex].instructions.rbegin();
         instructionIt != function.blocks[blockIndex].instructions.rend();
         ++instructionIt) {
      std::vector<MachineOperand> allUses;
      std::vector<MachineOperand> allDefs;
      adapter.enumerateUses(*instructionIt, allUses);
      adapter.enumerateDefs(*instructionIt, allDefs);
      const auto uses = virtualOperands(allUses, stackHomeParameters);
      const auto defs = virtualOperands(allDefs, stackHomeParameters);

      std::vector<std::size_t> useIndices;
      std::vector<std::size_t> defIndices;
      useIndices.reserve(uses.size());
      defIndices.reserve(defs.size());
      for (const auto& operand : defs) {
        const std::size_t index = graph.indexOf(operand.text);
        graph.nodes[index].spillCost += weight;
        defIndices.push_back(index);
      }
      for (const auto& operand : uses) {
        const std::size_t index = graph.indexOf(operand.text);
        graph.nodes[index].spillCost += weight;
        useIndices.push_back(index);
      }

      if (adapter.isCall(*instructionIt)) {
        forEachSetBit(live, graph.nodes.size(), [&](std::size_t index) {
          if (std::find(defIndices.begin(), defIndices.end(), index) ==
              defIndices.end()) {
            graph.nodes[index].crossesCall = true;
          }
        });
      }

      std::size_t copySource = graph.nodes.size();
      if (instructionIt->kind == MachineInstructionKind::Copy &&
          defs.size() == 1 && allUses.size() == 1) {
        const auto& source = allUses.front();
        if (isVirtualRegister(source) &&
            stackHomeParameters.find(source.text) ==
                stackHomeParameters.end()) {
          copySource = graph.indexOf(source.text);
          graph.addAffinity(defIndices.front(), copySource);
        } else if (source.kind == MachineOperandKind::PhysicalRegister) {
          graph.nodes[defIndices.front()].preferredRegisters.push_back(
              source.text);
        }
      }

      for (const auto def : defIndices) {
        forEachSetBit(live, graph.nodes.size(), [&](std::size_t liveNode) {
          if (liveNode != copySource) {
            graph.addEdge(def, liveNode);
          }
        });
      }

      for (const auto def : defIndices) {
        clearBit(live, def);
      }
      for (const auto use : useIndices) {
        setBit(live, use);
      }
    }
  }

  for (auto& node : graph.nodes) {
    if (!node.avoidArgumentRegisters) {
      continue;
    }
    if (!node.crossesCall && !node.preferredRegisters.empty()) {
      node.precoloredRegister = node.preferredRegisters.front();
    }
    node.avoidArgumentRegisters = false;
  }
}

std::vector<std::string> allowedRegisters(const GraphNode& node,
                                          const AllocationClass& klass) {
  std::vector<std::string> allowed;
  allowed.reserve(klass.allocatable.size());
  for (const auto& reg : klass.allocatable) {
    if (node.crossesCall &&
        klass.calleeSaved.find(reg) == klass.calleeSaved.end()) {
      continue;
    }
    if (node.avoidArgumentRegisters &&
        klass.argumentRegisters.find(reg) != klass.argumentRegisters.end()) {
      continue;
    }
    allowed.push_back(reg);
  }
  return allowed;
}

void allocateClassGraph(
    const InterferenceGraph& graph, const AllocationClass& klass,
    bool selectFloat,
    std::unordered_map<std::string, std::string>& registerMap) {
  std::vector<bool> remaining(graph.nodes.size(), false);
  std::vector<std::vector<std::string>> allowedByNode(graph.nodes.size());
  std::size_t remainingCount = 0;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    const auto& node = graph.nodes[index];
    if (node.type.isFloat() != selectFloat || !isRegisterCandidate(node.type)) {
      continue;
    }
    allowedByNode[index] = allowedRegisters(node, klass);
    if (!node.precoloredRegister.empty() &&
        std::find(allowedByNode[index].begin(), allowedByNode[index].end(),
                  node.precoloredRegister) != allowedByNode[index].end()) {
      registerMap.emplace(node.name, node.precoloredRegister);
      continue;
    }
    remaining[index] = true;
    ++remainingCount;
  }

  std::vector<std::size_t> degree(graph.nodes.size(), 0);
  std::vector<std::size_t> simplifyWorklist;
  using SpillCandidate = std::pair<double, std::size_t>;
  std::priority_queue<SpillCandidate, std::vector<SpillCandidate>,
                      std::greater<SpillCandidate>>
      spillWorklist;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    if (!remaining[index]) {
      continue;
    }
    std::size_t nodeDegree = 0;
    graph.forEachNeighbor(index, [&](std::size_t neighbor) {
      if (remaining[neighbor]) {
        ++nodeDegree;
      }
    });
    degree[index] = nodeDegree;
    if (nodeDegree < allowedByNode[index].size()) {
      simplifyWorklist.push_back(index);
    }
    const double spillScore = (graph.nodes[index].spillCost + 1.0) /
                              static_cast<double>(nodeDegree + 1);
    spillWorklist.emplace(spillScore, index);
  }

  std::vector<ColoringStackEntry> stack;
  stack.reserve(remainingCount);
  while (remainingCount > 0) {
    std::size_t selected = graph.nodes.size();
    while (!simplifyWorklist.empty()) {
      const std::size_t candidate = simplifyWorklist.back();
      simplifyWorklist.pop_back();
      if (remaining[candidate] &&
          degree[candidate] < allowedByNode[candidate].size()) {
        selected = candidate;
        break;
      }
    }

    if (selected == graph.nodes.size()) {
      while (!spillWorklist.empty() && !remaining[spillWorklist.top().second]) {
        spillWorklist.pop();
      }
      selected = spillWorklist.top().second;
      spillWorklist.pop();
    }

    stack.push_back(ColoringStackEntry{selected});
    remaining[selected] = false;
    --remainingCount;
    graph.forEachNeighbor(selected, [&](std::size_t neighbor) {
      if (remaining[neighbor]) {
        if (degree[neighbor] > 0) {
          --degree[neighbor];
        }
        if (degree[neighbor] < allowedByNode[neighbor].size()) {
          simplifyWorklist.push_back(neighbor);
        }
      }
    });
  }

  while (!stack.empty()) {
    const ColoringStackEntry entry = stack.back();
    stack.pop_back();
    const auto& node = graph.nodes[entry.node];
    std::unordered_set<std::string> unavailable;
    graph.forEachNeighbor(entry.node, [&](std::size_t neighbor) {
      if (const auto assigned = registerMap.find(graph.nodes[neighbor].name);
          assigned != registerMap.end()) {
        unavailable.insert(assigned->second);
      }
    });

    std::vector<std::string> preferred;
    for (const auto& affinity : node.affinities) {
      if (const auto assigned = registerMap.find(graph.nodes[affinity].name);
          assigned != registerMap.end()) {
        preferred.push_back(assigned->second);
      }
    }
    preferred.insert(preferred.end(), node.preferredRegisters.begin(),
                     node.preferredRegisters.end());

    auto ordered = allowedByNode[entry.node];
    std::sort(ordered.begin(), ordered.end(),
              [&](const std::string& lhs, const std::string& rhs) {
                const bool lhsPreferred =
                    std::find(preferred.begin(), preferred.end(), lhs) !=
                    preferred.end();
                const bool rhsPreferred =
                    std::find(preferred.begin(), preferred.end(), rhs) !=
                    preferred.end();
                if (lhsPreferred != rhsPreferred) {
                  return lhsPreferred > rhsPreferred;
                }
                const bool lhsSaved =
                    klass.calleeSaved.find(lhs) != klass.calleeSaved.end();
                const bool rhsSaved =
                    klass.calleeSaved.find(rhs) != klass.calleeSaved.end();
                if (lhsSaved != rhsSaved) {
                  return node.crossesCall ? lhsSaved > rhsSaved
                                          : lhsSaved < rhsSaved;
                }
                return lhs < rhs;
              });

    for (const auto& reg : ordered) {
      if (unavailable.find(reg) == unavailable.end()) {
        registerMap.emplace(node.name, reg);
        break;
      }
    }
  }
}

bool keepUseInFrame(const MachineInstruction& instruction) {
  return instruction.opcode == "call" || instruction.opcode == "ret" ||
         instruction.opcode == "br" ||
         instruction.kind == MachineInstructionKind::Phi;
}

std::string takeScratchRegister(
    const ValueType& type,
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

        if (const auto reg = registerMap.find(operand.text);
            reg != registerMap.end()) {
          operand = MachineOperand::physicalRegister(reg->second, operand.type);
          return;
        }

        const int slot =
            ensureSlot(function, slotMap, operand.text, operand.type, target);
        if (keepUseInFrame(rewritten)) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        const std::string scratch = takeScratchRegister(
            operand.type, regInfo, nextIntegerScratch, nextFloatScratch);
        if (scratch.empty()) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        auto physical = MachineOperand::physicalRegister(scratch, operand.type);
        before.push_back(MachineInstruction::spillReload(
            physical, MachineOperand::stackSlot(slot, operand.type)));
        operand = std::move(physical);
      };

      const auto rewriteDef = [&](MachineOperand& operand) {
        if (operand.kind != MachineOperandKind::VirtualRegister) {
          return;
        }

        if (const auto reg = registerMap.find(operand.text);
            reg != registerMap.end()) {
          operand = MachineOperand::physicalRegister(reg->second, operand.type);
          return;
        }

        const int slot =
            ensureSlot(function, slotMap, operand.text, operand.type, target);
        if (rewritten.kind == MachineInstructionKind::Copy ||
            rewritten.kind == MachineInstructionKind::Phi) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        const std::string scratch = takeScratchRegister(
            operand.type, regInfo, nextIntegerScratch, nextFloatScratch);
        if (scratch.empty()) {
          operand = MachineOperand::stackSlot(slot, operand.type);
          return;
        }

        auto physical = MachineOperand::physicalRegister(scratch, operand.type);
        operand = physical;
        after.push_back(MachineInstruction::spillStore(
            std::move(physical),
            MachineOperand::stackSlot(slot, operand.type)));
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

void materializeRegisterParameters(
    MachineFunction& function,
    std::unordered_set<std::string>& stackHomeParameters,
    std::unordered_set<std::string>& registerHomeParameters,
    std::unordered_map<std::string, int>& slotMap,
    const compiler::backend::target::TargetInfo& target,
    const compiler::backend::target::TargetRegInfo& regInfo) {
  if (function.blocks.empty()) {
    return;
  }

  std::vector<MachineInstruction> copies;
  std::size_t intRegs = 0;
  std::size_t floatRegs = 0;
  for (auto& parameter : function.parameters) {
    const bool isFloat = parameter.type.isFloat();
    const auto& argRegs = isFloat ? regInfo.floatArgumentRegisters()
                                  : regInfo.integerArgumentRegisters();
    auto& regIndex = isFloat ? floatRegs : intRegs;
    if (regIndex < argRegs.size()) {
      parameter.homeSlot = -1;
      registerHomeParameters.insert(parameter.name);
      copies.push_back(MachineInstruction::copy(
          MachineOperand::virtualRegister(parameter.name, parameter.type),
          MachineOperand::physicalRegister(argRegs[regIndex], parameter.type)));
      ++regIndex;
      continue;
    }

    parameter.homeSlot =
        ensureSlot(function, slotMap, parameter.name, parameter.type, target);
    stackHomeParameters.insert(parameter.name);
    ++regIndex;
  }

  auto& entryInstructions = function.blocks.front().instructions;
  entryInstructions.insert(entryInstructions.begin(), copies.begin(),
                           copies.end());
}

}  // namespace

void RegisterAllocator::allocate(
    compiler::backend::mir::MachineModule& module) const {
  const auto& adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  const auto& regInfo = compiler::backend::target::regInfoFor(module.target);

  for (auto& function : module.functions) {
    const auto allocationStart = std::chrono::steady_clock::now();
    std::unordered_map<std::string, int> slotMap;
    std::unordered_set<std::string> stackHomeParameters;
    std::unordered_set<std::string> registerHomeParameters;
    materializeRegisterParameters(function, stackHomeParameters,
                                  registerHomeParameters, slotMap,
                                  module.target, regInfo);

    std::unordered_map<std::string, ValueType> regTypes;
    std::vector<std::unordered_set<std::string>> useSets;
    std::vector<std::unordered_set<std::string>> defSets;
    collectBlockUsesAndDefs(adapter, function, stackHomeParameters, regTypes,
                            useSets, defSets);

    InterferenceGraph graph;
    for (const auto& [name, type] : regTypes) {
      auto& node = graph.addNode(name, type);
      if (registerHomeParameters.find(name) != registerHomeParameters.end()) {
        node.avoidArgumentRegisters = true;
      }
    }
    graph.finalizeNodes();

    const std::size_t registerWordCount = (graph.nodes.size() + 63) / 64;
    std::vector<BitVector> blockUses(
        function.blocks.size(), BitVector(registerWordCount, 0));
    std::vector<BitVector> blockDefs(
        function.blocks.size(), BitVector(registerWordCount, 0));
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
         ++blockIndex) {
      for (const auto& name : useSets[blockIndex]) {
        setBit(blockUses[blockIndex], graph.indexOf(name));
      }
      for (const auto& name : defSets[blockIndex]) {
        setBit(blockDefs[blockIndex], graph.indexOf(name));
      }
    }
    useSets.clear();
    defSets.clear();

    const auto cfg = CFGBuilder{}.build(function, adapter);
    std::vector<BitVector> liveIn(
        function.blocks.size(), BitVector(registerWordCount, 0));
    std::vector<BitVector> liveOut(
        function.blocks.size(), BitVector(registerWordCount, 0));
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t reverse = function.blocks.size(); reverse > 0;
           --reverse) {
        const std::size_t blockIndex = reverse - 1;

        BitVector newOut(registerWordCount, 0);
        for (const auto succ : cfg.successors[blockIndex]) {
          for (std::size_t word = 0; word < registerWordCount; ++word) {
            newOut[word] |= liveIn[succ][word];
          }
        }

        BitVector newIn(registerWordCount, 0);
        for (std::size_t word = 0; word < registerWordCount; ++word) {
          newIn[word] =
              blockUses[blockIndex][word] |
              (newOut[word] & ~blockDefs[blockIndex][word]);
        }

        if (newOut != liveOut[blockIndex] || newIn != liveIn[blockIndex]) {
          liveOut[blockIndex] = std::move(newOut);
          liveIn[blockIndex] = std::move(newIn);
          changed = true;
        }
      }
    }
    const auto livenessEnd = std::chrono::steady_clock::now();

    buildInterferenceGraph(graph, adapter, function, cfg, stackHomeParameters,
                           liveOut);
    const auto graphEnd = std::chrono::steady_clock::now();

    std::unordered_map<std::string, std::string> registerMap;
    allocateClassGraph(
        graph,
        AllocationClass{regInfo.allocatableIntegerRegisters(),
                        std::unordered_set<std::string>(
                            regInfo.calleeSavedIntegerRegisters().begin(),
                            regInfo.calleeSavedIntegerRegisters().end()),
                        std::unordered_set<std::string>(
                            regInfo.integerArgumentRegisters().begin(),
                            regInfo.integerArgumentRegisters().end())},
        false, registerMap);
    allocateClassGraph(
        graph,
        AllocationClass{regInfo.allocatableFloatRegisters(),
                        std::unordered_set<std::string>(
                            regInfo.calleeSavedFloatRegisters().begin(),
                            regInfo.calleeSavedFloatRegisters().end()),
                        std::unordered_set<std::string>(
                            regInfo.floatArgumentRegisters().begin(),
                            regInfo.floatArgumentRegisters().end())},
        true, registerMap);
    const auto coloringEnd = std::chrono::steady_clock::now();

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
    function.usedIntegerCalleeSavedRegisters.assign(usedRegs.begin(),
                                                    usedRegs.end());
    std::sort(function.usedIntegerCalleeSavedRegisters.begin(),
              function.usedIntegerCalleeSavedRegisters.end());
    function.usedFloatCalleeSavedRegisters.assign(usedFloatRegs.begin(),
                                                  usedFloatRegs.end());
    std::sort(function.usedFloatCalleeSavedRegisters.begin(),
              function.usedFloatCalleeSavedRegisters.end());

    rewriteFunctionInstructions(function, slotMap, registerMap, module.target,
                                regInfo);
    if (std::getenv("COMPILER_REGALLOC_STATS") != nullptr) {
      const auto rewriteEnd = std::chrono::steady_clock::now();
      const auto milliseconds = [](const auto begin, const auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
      };
      std::cerr << "regalloc " << function.name
                << ": nodes=" << graph.nodes.size()
                << " liveness_ms=" << milliseconds(allocationStart, livenessEnd)
                << " graph_ms=" << milliseconds(livenessEnd, graphEnd)
                << " color_ms=" << milliseconds(graphEnd, coloringEnd)
                << " rewrite_ms=" << milliseconds(coloringEnd, rewriteEnd)
                << '\n';
    }
  }
}

}  // namespace compiler::backend::regalloc
