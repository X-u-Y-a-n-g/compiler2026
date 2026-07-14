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
  struct Affinity {
    std::size_t node = 0;
    double weight = 0.0;
  };

  struct RegisterPreference {
    std::string name;
    double weight = 0.0;
  };

  std::string name;
  ValueType type;
  std::vector<Affinity> affinities;
  std::vector<RegisterPreference> preferredRegisters;
  std::string precoloredRegister;
  double spillCost = 0.0;
  bool crossesCall = false;
  bool isRegisterParameter = false;
};

struct InterferenceGraph {
  std::vector<GraphNode> nodes;
  std::unordered_map<std::string, std::size_t> indexByName;
  std::vector<std::uint64_t> adjacency;
  std::vector<std::unordered_set<std::size_t>> sparseAdjacencyBuilder;
  std::vector<std::vector<std::size_t>> sparseAdjacency;
  std::size_t wordsPerNode = 0;
  bool usesDenseAdjacency = true;

  std::size_t ensureNode(const std::string& name, const ValueType& type) {
    const auto found = indexByName.find(name);
    if (found != indexByName.end()) {
      auto& node = nodes[found->second];
      if (node.type.isVoid()) {
        node.type = type;
      }
      return found->second;
    }
    const std::size_t index = nodes.size();
    indexByName.emplace(name, index);
    nodes.push_back(GraphNode{name, type, {}, {}, {}, 0.0, false, false});
    return index;
  }

  [[nodiscard]] std::size_t indexOf(const std::string& name) const {
    return indexByName.at(name);
  }

  void finalizeNodes() {
    wordsPerNode = (nodes.size() + 63) / 64;
    constexpr std::size_t kDenseAdjacencyBudget = 256U * 1024U * 1024U;
    const bool denseSizeFits =
        wordsPerNode == 0 ||
        nodes.size() <=
            kDenseAdjacencyBudget / (wordsPerNode * sizeof(std::uint64_t));
    const bool forceSparse =
        std::getenv("COMPILER_REGALLOC_FORCE_SPARSE") != nullptr;
    usesDenseAdjacency = denseSizeFits && !forceSparse;
    if (usesDenseAdjacency) {
      adjacency.assign(nodes.size() * wordsPerNode, 0);
    } else {
      sparseAdjacencyBuilder.resize(nodes.size());
    }
  }

  void addEdge(std::size_t lhs, std::size_t rhs) {
    if (lhs == rhs || nodes[lhs].type.isFloat() != nodes[rhs].type.isFloat()) {
      return;
    }
    if (usesDenseAdjacency) {
      adjacency[lhs * wordsPerNode + rhs / 64] |= std::uint64_t{1}
                                                  << (rhs % 64);
      adjacency[rhs * wordsPerNode + lhs / 64] |= std::uint64_t{1}
                                                  << (lhs % 64);
    } else {
      sparseAdjacencyBuilder[lhs].insert(rhs);
      sparseAdjacencyBuilder[rhs].insert(lhs);
    }
  }

  void freezeEdges() {
    if (usesDenseAdjacency) {
      return;
    }
    sparseAdjacency.resize(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      auto& compact = sparseAdjacency[index];
      compact.assign(sparseAdjacencyBuilder[index].begin(),
                     sparseAdjacencyBuilder[index].end());
      std::sort(compact.begin(), compact.end());
    }
    sparseAdjacencyBuilder.clear();
    sparseAdjacencyBuilder.shrink_to_fit();
  }

  void addAffinity(std::size_t lhs, std::size_t rhs, double weight) {
    if (lhs == rhs || nodes[lhs].type.isFloat() != nodes[rhs].type.isFloat()) {
      return;
    }
    const auto addOneWay = [&](std::size_t from, std::size_t to) {
      auto& affinities = nodes[from].affinities;
      const auto found = std::find_if(
          affinities.begin(), affinities.end(),
          [&](const GraphNode::Affinity& affinity) { return affinity.node == to; });
      if (found == affinities.end()) {
        affinities.push_back(GraphNode::Affinity{to, weight});
      } else {
        found->weight += weight;
      }
    };
    addOneWay(lhs, rhs);
    addOneWay(rhs, lhs);
  }

  void addRegisterPreference(std::size_t node, const std::string& reg,
                             double weight) {
    auto& preferences = nodes[node].preferredRegisters;
    const auto found = std::find_if(
        preferences.begin(), preferences.end(),
        [&](const GraphNode::RegisterPreference& preference) {
          return preference.name == reg;
        });
    if (found == preferences.end()) {
      preferences.push_back(GraphNode::RegisterPreference{reg, weight});
    } else {
      found->weight += weight;
    }
  }

  template <typename Callback>
  void forEachNeighbor(std::size_t index, Callback&& callback) const {
    if (!usesDenseAdjacency) {
      if (!sparseAdjacency.empty()) {
        for (const auto neighbor : sparseAdjacency[index]) {
          callback(neighbor);
        }
      } else {
        for (const auto neighbor : sparseAdjacencyBuilder[index]) {
          callback(neighbor);
        }
      }
      return;
    }
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
    InterferenceGraph& graph,
    std::vector<std::unordered_set<std::size_t>>& useSets,
    std::vector<std::unordered_set<std::size_t>>& defSets) {
  useSets.assign(function.blocks.size(), {});
  defSets.assign(function.blocks.size(), {});

  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    std::unordered_set<std::size_t> seenDefs;
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
        const std::size_t index = graph.ensureNode(operand.text, operand.type);
        if (seenDefs.find(index) == seenDefs.end()) {
          useSets[blockIndex].insert(index);
        }
      }

      for (const auto& operand : defs) {
        if (!isVirtualRegister(operand) ||
            parameterNames.find(operand.text) != parameterNames.end()) {
          continue;
        }
        const std::size_t index = graph.ensureNode(operand.text, operand.type);
        defSets[blockIndex].insert(index);
        seenDefs.insert(index);
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
  std::vector<std::uint64_t> dominators(blockCount * wordCount,
                                        ~std::uint64_t{0});
  std::fill_n(dominators.begin(), wordCount, 0);
  dominators[0] = 1;
  for (std::size_t block = 1; block < blockCount; ++block) {
    dominators[block * wordCount + wordCount - 1] &= lastWordMask;
  }

  std::vector<std::uint64_t> updated(wordCount, 0);
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < blockCount; ++block) {
      if (cfg.predecessors[block].empty()) {
        // An unreachable block is only dominated by itself.
        std::fill(updated.begin(), updated.end(), 0);
      } else {
        const std::size_t firstPredOffset =
            cfg.predecessors[block].front() * wordCount;
        std::copy_n(dominators.begin() + firstPredOffset, wordCount,
                    updated.begin());
        for (std::size_t predIndex = 1;
             predIndex < cfg.predecessors[block].size(); ++predIndex) {
          const std::size_t predOffset =
              cfg.predecessors[block][predIndex] * wordCount;
          for (std::size_t word = 0; word < wordCount; ++word) {
            updated[word] &= dominators[predOffset + word];
          }
        }
      }
      updated[block / 64] |= std::uint64_t{1} << (block % 64);
      const std::size_t blockOffset = block * wordCount;
      if (!std::equal(updated.begin(), updated.end(),
                      dominators.begin() + blockOffset)) {
        std::copy(updated.begin(), updated.end(),
                  dominators.begin() + blockOffset);
        changed = true;
      }
    }
  }

  std::vector<unsigned char> inNaturalLoop(blockCount, 0);
  std::vector<std::size_t> naturalLoopBlocks;
  std::vector<std::size_t> worklist;
  for (std::size_t tail = 0; tail < blockCount; ++tail) {
    for (const auto head : cfg.successors[tail]) {
      if ((dominators[tail * wordCount + head / 64] &
           (std::uint64_t{1} << (head % 64))) == 0) {
        continue;
      }

      naturalLoopBlocks.clear();
      worklist.clear();
      inNaturalLoop[head] = 1;
      naturalLoopBlocks.push_back(head);
      if (tail != head) {
        inNaturalLoop[tail] = 1;
        naturalLoopBlocks.push_back(tail);
      }
      if (tail != head) {
        worklist.push_back(tail);
      }
      while (!worklist.empty()) {
        const std::size_t block = worklist.back();
        worklist.pop_back();
        for (const auto pred : cfg.predecessors[block]) {
          if (inNaturalLoop[pred] == 0) {
            inNaturalLoop[pred] = 1;
            naturalLoopBlocks.push_back(pred);
            if (pred != head) {
              worklist.push_back(pred);
            }
          }
        }
      }
      for (const auto block : naturalLoopBlocks) {
        ++loopDepth[block];
        inNaturalLoop[block] = 0;
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
    const compiler::backend::target::TargetRegInfo& regInfo,
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

        std::size_t integerArgument = 0;
        std::size_t floatArgument = 0;
        for (std::size_t operandIndex = 1;
             operandIndex < instructionIt->operands.size(); ++operandIndex) {
          const auto& operand = instructionIt->operands[operandIndex];
          auto& argumentIndex =
              operand.type.isFloat() ? floatArgument : integerArgument;
          const auto& argumentRegisters =
              operand.type.isFloat() ? regInfo.floatArgumentRegisters()
                                     : regInfo.integerArgumentRegisters();
          if (isVirtualRegister(operand) &&
              stackHomeParameters.find(operand.text) ==
                  stackHomeParameters.end() &&
              argumentIndex < argumentRegisters.size()) {
            graph.addRegisterPreference(graph.indexOf(operand.text),
                                        argumentRegisters[argumentIndex],
                                        weight);
          }
          ++argumentIndex;
        }
        if (defIndices.size() == 1) {
          const auto& returnRegisters =
              defs.front().type.isFloat() ? regInfo.floatArgumentRegisters()
                                          : regInfo.integerArgumentRegisters();
          if (!returnRegisters.empty()) {
            graph.addRegisterPreference(defIndices.front(),
                                        returnRegisters.front(), weight);
          }
        }
      } else if (instructionIt->opcode == "ret" && useIndices.size() == 1) {
        const auto& returnRegisters =
            uses.front().type.isFloat() ? regInfo.floatArgumentRegisters()
                                        : regInfo.integerArgumentRegisters();
        if (!returnRegisters.empty()) {
          graph.addRegisterPreference(useIndices.front(),
                                      returnRegisters.front(), weight);
        }
      }

      std::size_t copySource = graph.nodes.size();
      if (instructionIt->kind == MachineInstructionKind::Copy &&
          defs.size() == 1 && allUses.size() == 1) {
        const auto& source = allUses.front();
        if (isVirtualRegister(source) &&
            stackHomeParameters.find(source.text) ==
                stackHomeParameters.end()) {
          copySource = graph.indexOf(source.text);
          graph.addAffinity(defIndices.front(), copySource, weight);
        } else if (source.kind == MachineOperandKind::PhysicalRegister) {
          graph.addRegisterPreference(defIndices.front(), source.text, weight);
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
    if (!node.isRegisterParameter) {
      continue;
    }
    if (!node.crossesCall && !node.preferredRegisters.empty()) {
      node.precoloredRegister =
          std::max_element(
              node.preferredRegisters.begin(), node.preferredRegisters.end(),
              [](const GraphNode::RegisterPreference& lhs,
                 const GraphNode::RegisterPreference& rhs) {
                return lhs.weight < rhs.weight;
              })
              ->name;
    }
  }
  graph.freezeEdges();
}

struct ColorPalette {
  std::vector<std::string> registers;
  std::unordered_map<std::string, unsigned> indexByName;
  std::uint64_t allMask = 0;
  std::uint64_t calleeSavedMask = 0;

  explicit ColorPalette(const AllocationClass& klass)
      : registers(klass.allocatable) {
    for (unsigned index = 0; index < registers.size(); ++index) {
      indexByName.emplace(registers[index], index);
      allMask |= std::uint64_t{1} << index;
      if (klass.calleeSaved.find(registers[index]) != klass.calleeSaved.end()) {
        calleeSavedMask |= std::uint64_t{1} << index;
      }
    }
  }
};

std::uint64_t allowedColorMask(const GraphNode&,
                               const ColorPalette& palette) {
  return palette.allMask;
}

std::size_t colorCount(std::uint64_t colors) {
  return static_cast<std::size_t>(__builtin_popcountll(colors));
}

int chooseWeightedColor(std::uint64_t colors,
                        const std::vector<double>& weights,
                        const ColorPalette& palette,
                        bool preferCalleeSaved) {
  if (colors == 0) {
    return -1;
  }
  const std::uint64_t preferredGroup = preferCalleeSaved
                                           ? colors & palette.calleeSavedMask
                                           : colors & ~palette.calleeSavedMask;
  std::uint64_t candidates = preferredGroup != 0 ? preferredGroup : colors;
  int bestColor = -1;
  double bestWeight = -1.0;
  while (candidates != 0) {
    const int color = static_cast<int>(__builtin_ctzll(candidates));
    if (weights[static_cast<std::size_t>(color)] > bestWeight) {
      bestColor = color;
      bestWeight = weights[static_cast<std::size_t>(color)];
    }
    candidates &= candidates - 1;
  }
  return bestColor;
}

void allocateClassGraph(
    const InterferenceGraph& graph, const AllocationClass& klass,
    bool selectFloat,
    std::unordered_map<std::string, std::string>& registerMap) {
  const ColorPalette palette(klass);
  std::vector<unsigned char> remaining(graph.nodes.size(), 0);
  std::vector<std::uint64_t> allowedByNode(graph.nodes.size(), 0);
  std::vector<int> assignedColor(graph.nodes.size(), -1);
  std::size_t remainingCount = 0;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    const auto& node = graph.nodes[index];
    if (node.type.isFloat() != selectFloat || !isRegisterCandidate(node.type)) {
      continue;
    }
    allowedByNode[index] = allowedColorMask(node, palette);
    if (!node.precoloredRegister.empty()) {
      const auto precolored = palette.indexByName.find(node.precoloredRegister);
      if (precolored != palette.indexByName.end() &&
          (allowedByNode[index] & (std::uint64_t{1} << precolored->second)) !=
              0) {
        assignedColor[index] = static_cast<int>(precolored->second);
        registerMap.emplace(node.name, node.precoloredRegister);
        continue;
      }
    }
    remaining[index] = 1;
    ++remainingCount;
  }

  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    if (remaining[index] == 0) {
      continue;
    }
    std::uint64_t fixedUnavailable = 0;
    graph.forEachNeighbor(index, [&](std::size_t neighbor) {
      if (assignedColor[neighbor] >= 0) {
        fixedUnavailable |= std::uint64_t{1} << assignedColor[neighbor];
      }
    });
    allowedByNode[index] &= ~fixedUnavailable;
  }

  std::vector<std::size_t> degree(graph.nodes.size(), 0);
  std::vector<std::size_t> simplifyWorklist;
  using SpillCandidate = std::pair<double, std::size_t>;
  std::priority_queue<SpillCandidate, std::vector<SpillCandidate>,
                      std::greater<SpillCandidate>>
      spillWorklist;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    if (remaining[index] == 0) {
      continue;
    }
    std::size_t nodeDegree = 0;
    graph.forEachNeighbor(index, [&](std::size_t neighbor) {
      if (remaining[neighbor] != 0) {
        ++nodeDegree;
      }
    });
    degree[index] = nodeDegree;
    if (nodeDegree < colorCount(allowedByNode[index])) {
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
      if (remaining[candidate] != 0 &&
          degree[candidate] < colorCount(allowedByNode[candidate])) {
        selected = candidate;
        break;
      }
    }

    if (selected == graph.nodes.size()) {
      while (!spillWorklist.empty() &&
             remaining[spillWorklist.top().second] == 0) {
        spillWorklist.pop();
      }
      selected = spillWorklist.top().second;
      spillWorklist.pop();
    }

    stack.push_back(ColoringStackEntry{selected});
    remaining[selected] = 0;
    --remainingCount;
    graph.forEachNeighbor(selected, [&](std::size_t neighbor) {
      if (remaining[neighbor] != 0) {
        if (degree[neighbor] > 0) {
          --degree[neighbor];
        }
        if (degree[neighbor] < colorCount(allowedByNode[neighbor])) {
          simplifyWorklist.push_back(neighbor);
        }
      }
    });
  }

  while (!stack.empty()) {
    const ColoringStackEntry entry = stack.back();
    stack.pop_back();
    const auto& node = graph.nodes[entry.node];
    std::uint64_t unavailable = 0;
    graph.forEachNeighbor(entry.node, [&](std::size_t neighbor) {
      if (assignedColor[neighbor] >= 0) {
        unavailable |= std::uint64_t{1} << assignedColor[neighbor];
      }
    });

    std::vector<double> preferenceWeights(palette.registers.size(), 0.0);
    for (const auto& affinity : node.affinities) {
      if (assignedColor[affinity.node] >= 0) {
        preferenceWeights[static_cast<std::size_t>(
            assignedColor[affinity.node])] += affinity.weight;
      }
    }
    for (const auto& preference : node.preferredRegisters) {
      if (const auto found = palette.indexByName.find(preference.name);
          found != palette.indexByName.end()) {
        preferenceWeights[found->second] += preference.weight;
      }
    }

    const std::uint64_t available = allowedByNode[entry.node] & ~unavailable;
    const int color = chooseWeightedColor(available, preferenceWeights, palette,
                                          node.crossesCall);
    if (color >= 0) {
      assignedColor[entry.node] = color;
      registerMap.emplace(node.name,
                          palette.registers[static_cast<std::size_t>(color)]);
    }
  }

  // Greedy coloring only sees affinity partners that have already received a
  // color.  In particular, phi copies on loop backedges are often colored in
  // the opposite order and miss an otherwise legal coalescing opportunity.
  // Revisit colored nodes and move them to an affinity partner's color when
  // that color is free of interference.  This preserves the graph-coloring
  // constraints while eliminating copies without speculative live-range
  // merging.
  constexpr unsigned kAffinityRecolorPasses = 4;
  for (unsigned pass = 0; pass < kAffinityRecolorPasses; ++pass) {
    bool changed = false;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
      const auto& node = graph.nodes[index];
      if (node.type.isFloat() != selectFloat || assignedColor[index] < 0 ||
          !node.precoloredRegister.empty()) {
        continue;
      }

      std::uint64_t unavailable = 0;
      graph.forEachNeighbor(index, [&](std::size_t neighbor) {
        if (assignedColor[neighbor] >= 0) {
          unavailable |= std::uint64_t{1} << assignedColor[neighbor];
        }
      });
      std::uint64_t available = allowedByNode[index] & ~unavailable;
      const int current = assignedColor[index];
      const bool currentIsCalleeSaved =
          (palette.calleeSavedMask & (std::uint64_t{1} << current)) != 0;
      const std::uint64_t sameSaveClass =
          currentIsCalleeSaved ? palette.calleeSavedMask
                               : palette.allMask & ~palette.calleeSavedMask;
      available &= sameSaveClass;
      if (available == 0) {
        continue;
      }

      std::vector<double> weights(palette.registers.size(), 0.0);
      for (const auto& affinity : node.affinities) {
        if (assignedColor[affinity.node] >= 0) {
          weights[static_cast<std::size_t>(assignedColor[affinity.node])] +=
              affinity.weight;
        }
      }
      for (const auto& preference : node.preferredRegisters) {
        if (const auto found = palette.indexByName.find(preference.name);
            found != palette.indexByName.end()) {
          weights[found->second] += preference.weight;
        }
      }

      int best = current;
      double bestWeight = weights[static_cast<std::size_t>(current)];
      std::uint64_t candidates = available;
      while (candidates != 0) {
        const int candidate = static_cast<int>(__builtin_ctzll(candidates));
        const double candidateWeight =
            weights[static_cast<std::size_t>(candidate)];
        if (candidateWeight > bestWeight) {
          best = candidate;
          bestWeight = candidateWeight;
        }
        candidates &= candidates - 1;
      }
      if (best != current) {
        assignedColor[index] = best;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }

  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    if (graph.nodes[index].type.isFloat() == selectFloat &&
        assignedColor[index] >= 0) {
      registerMap[graph.nodes[index].name] =
          palette.registers[static_cast<std::size_t>(assignedColor[index])];
    }
  }
}

bool keepUseInFrame(const MachineInstruction& instruction) {
  return instruction.opcode == "call" || instruction.opcode == "ret" ||
         instruction.opcode == "br" ||
         instruction.kind == MachineInstructionKind::Phi;
}

void insertCallerSaves(
    MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter,
    const InterferenceGraph& graph, const std::vector<BitVector>& liveOut,
    const std::unordered_set<std::string>& stackHomeParameters,
    const std::unordered_map<std::string, std::string>& registerMap,
    std::unordered_map<std::string, int>& slotMap,
    const compiler::backend::target::TargetInfo& target,
    const compiler::backend::target::TargetRegInfo& regInfo) {
  std::unordered_set<std::string> calleeSaved(
      regInfo.calleeSavedIntegerRegisters().begin(),
      regInfo.calleeSavedIntegerRegisters().end());
  calleeSaved.insert(regInfo.calleeSavedFloatRegisters().begin(),
                     regInfo.calleeSavedFloatRegisters().end());

  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    auto& block = function.blocks[blockIndex];
    std::vector<std::vector<std::size_t>> saves(block.instructions.size());
    BitVector live = liveOut[blockIndex];
    for (std::size_t instructionIndex = block.instructions.size();
         instructionIndex-- > 0;) {
      const auto& instruction = block.instructions[instructionIndex];
      std::vector<MachineOperand> allUses;
      std::vector<MachineOperand> allDefs;
      adapter.enumerateUses(instruction, allUses);
      adapter.enumerateDefs(instruction, allDefs);
      const auto uses = virtualOperands(allUses, stackHomeParameters);
      const auto defs = virtualOperands(allDefs, stackHomeParameters);
      std::vector<std::size_t> defIndices;
      defIndices.reserve(defs.size());
      for (const auto& operand : defs) {
        defIndices.push_back(graph.indexOf(operand.text));
      }

      if (adapter.isCall(instruction)) {
        forEachSetBit(live, graph.nodes.size(), [&](std::size_t nodeIndex) {
          if (std::find(defIndices.begin(), defIndices.end(), nodeIndex) !=
              defIndices.end()) {
            return;
          }
          const auto assigned = registerMap.find(graph.nodes[nodeIndex].name);
          if (assigned != registerMap.end() &&
              calleeSaved.find(assigned->second) == calleeSaved.end()) {
            saves[instructionIndex].push_back(nodeIndex);
          }
        });
      }

      for (const auto def : defIndices) {
        clearBit(live, def);
      }
      for (const auto& operand : uses) {
        setBit(live, graph.indexOf(operand.text));
      }
    }

    std::vector<MachineInstruction> rewritten;
    rewritten.reserve(block.instructions.size());
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      for (const auto nodeIndex : saves[instructionIndex]) {
        const auto& node = graph.nodes[nodeIndex];
        const int slot =
            ensureSlot(function, slotMap, node.name, node.type, target);
        rewritten.push_back(MachineInstruction::spillStore(
            MachineOperand::virtualRegister(node.name, node.type),
            MachineOperand::stackSlot(slot, node.type)));
      }
      rewritten.push_back(std::move(block.instructions[instructionIndex]));
      for (const auto nodeIndex : saves[instructionIndex]) {
        const auto& node = graph.nodes[nodeIndex];
        const int slot =
            ensureSlot(function, slotMap, node.name, node.type, target);
        rewritten.push_back(MachineInstruction::spillReload(
            MachineOperand::virtualRegister(node.name, node.type),
            MachineOperand::stackSlot(slot, node.type)));
      }
    }
    block.instructions = std::move(rewritten);
  }
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

    InterferenceGraph graph;
    std::vector<std::unordered_set<std::size_t>> useSets;
    std::vector<std::unordered_set<std::size_t>> defSets;
    collectBlockUsesAndDefs(adapter, function, stackHomeParameters, graph,
                            useSets, defSets);
    for (const auto& name : registerHomeParameters) {
      if (const auto found = graph.indexByName.find(name);
          found != graph.indexByName.end()) {
        graph.nodes[found->second].isRegisterParameter = true;
      }
    }
    graph.finalizeNodes();

    const std::size_t registerWordCount = (graph.nodes.size() + 63) / 64;
    std::vector<BitVector> blockUses(function.blocks.size(),
                                     BitVector(registerWordCount, 0));
    std::vector<BitVector> blockDefs(function.blocks.size(),
                                     BitVector(registerWordCount, 0));
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
         ++blockIndex) {
      for (const auto index : useSets[blockIndex]) {
        setBit(blockUses[blockIndex], index);
      }
      for (const auto index : defSets[blockIndex]) {
        setBit(blockDefs[blockIndex], index);
      }
    }
    useSets.clear();
    defSets.clear();

    const auto cfg = CFGBuilder{}.build(function, adapter);
    std::vector<BitVector> liveIn(function.blocks.size(),
                                  BitVector(registerWordCount, 0));
    std::vector<BitVector> liveOut(function.blocks.size(),
                                   BitVector(registerWordCount, 0));
    std::vector<std::size_t> livenessWorklist;
    livenessWorklist.reserve(function.blocks.size());
    std::vector<unsigned char> inWorklist(function.blocks.size(), 1);
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
         ++blockIndex) {
      livenessWorklist.push_back(blockIndex);
    }
    BitVector newOut(registerWordCount, 0);
    BitVector newIn(registerWordCount, 0);
    while (!livenessWorklist.empty()) {
      const std::size_t blockIndex = livenessWorklist.back();
      livenessWorklist.pop_back();
      inWorklist[blockIndex] = 0;

      std::fill(newOut.begin(), newOut.end(), 0);
      for (const auto succ : cfg.successors[blockIndex]) {
        for (std::size_t word = 0; word < registerWordCount; ++word) {
          newOut[word] |= liveIn[succ][word];
        }
      }
      for (std::size_t word = 0; word < registerWordCount; ++word) {
        newIn[word] = blockUses[blockIndex][word] |
                      (newOut[word] & ~blockDefs[blockIndex][word]);
      }

      const bool inputChanged = newIn != liveIn[blockIndex];
      if (newOut != liveOut[blockIndex]) {
        liveOut[blockIndex] = newOut;
      }
      if (!inputChanged) {
        continue;
      }
      liveIn[blockIndex] = newIn;
      for (const auto pred : cfg.predecessors[blockIndex]) {
        if (inWorklist[pred] == 0) {
          livenessWorklist.push_back(pred);
          inWorklist[pred] = 1;
        }
      }
    }
    const auto livenessEnd = std::chrono::steady_clock::now();

    buildInterferenceGraph(graph, adapter, regInfo, function, cfg,
                           stackHomeParameters, liveOut);
    const auto graphEnd = std::chrono::steady_clock::now();

    std::unordered_map<std::string, std::string> registerMap;
    allocateClassGraph(
        graph,
        AllocationClass{regInfo.allocatableIntegerRegisters(),
                        std::unordered_set<std::string>(
                            regInfo.calleeSavedIntegerRegisters().begin(),
                            regInfo.calleeSavedIntegerRegisters().end())},
        false, registerMap);
    allocateClassGraph(
        graph,
        AllocationClass{regInfo.allocatableFloatRegisters(),
                        std::unordered_set<std::string>(
                            regInfo.calleeSavedFloatRegisters().begin(),
                            regInfo.calleeSavedFloatRegisters().end())},
        true, registerMap);
    const auto coloringEnd = std::chrono::steady_clock::now();

    insertCallerSaves(function, adapter, graph, liveOut, stackHomeParameters,
                      registerMap, slotMap, module.target, regInfo);

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
                << " colored=" << registerMap.size()
                << " spilled=" << (graph.nodes.size() - registerMap.size())
                << " graph=" << (graph.usesDenseAdjacency ? "dense" : "sparse")
                << " liveness_ms=" << milliseconds(allocationStart, livenessEnd)
                << " graph_ms=" << milliseconds(livenessEnd, graphEnd)
                << " color_ms=" << milliseconds(graphEnd, coloringEnd)
                << " rewrite_ms=" << milliseconds(coloringEnd, rewriteEnd)
                << '\n';
    }
  }
}

}  // namespace compiler::backend::regalloc
