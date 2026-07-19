#include "../../../include/compiler/midend/pass/LoopCanonicalizationPass.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/analysis/LoopAnalysis.h"
#include "../../../include/compiler/ir/core/BasicBlock.h"
#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Instruction.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::analysis::LoopAnalysis;
using compiler::ir::analysis::NaturalLoop;
using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) names.insert(block.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "%" + stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

bool redirectBranch(BasicBlock& block, const std::string& oldTarget,
                    const std::string& newTarget) {
  if (block.instructions().empty()) return false;
  const auto& terminator = block.instructions().back();
  if (terminator.opcode() != "br") return false;
  auto operands = terminator.operands();
  bool changed = false;
  const std::size_t begin = operands.size() == 1 ? 0 : 1;
  for (std::size_t index = begin; index < operands.size(); ++index) {
    if (operands[index] == oldTarget) {
      operands[index] = newTarget;
      changed = true;
    }
  }
  if (changed) {
    block.instructions().back() = Instruction{"br", std::move(operands)};
  }
  return changed;
}

std::vector<std::pair<std::string, std::string>> phiIncomingFrom(
    const Instruction& phi, const std::unordered_set<std::string>& predecessors) {
  std::vector<std::pair<std::string, std::string>> incoming;
  if (phi.opcode() != "phi") return incoming;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (predecessors.find(phi.operands()[index + 1]) != predecessors.end()) {
      incoming.emplace_back(phi.operands()[index], phi.operands()[index + 1]);
    }
  }
  return incoming;
}

bool mergePhiEdges(Function& function, BasicBlock& destination,
                   const std::vector<std::string>& oldPredecessors,
                   const std::string& mergedPredecessor,
                   BasicBlock& mergeBlock, const std::string& valueStem) {
  const std::unordered_set<std::string> oldSet(oldPredecessors.begin(),
                                               oldPredecessors.end());
  std::vector<Instruction> rewritten;
  rewritten.reserve(destination.instructions().size());
  for (const auto& instruction : destination.instructions()) {
    if (instruction.opcode() != "phi") {
      rewritten.push_back(instruction);
      continue;
    }
    const auto incoming = phiIncomingFrom(instruction, oldSet);
    if (incoming.size() != oldPredecessors.size()) return false;
    std::string mergedValue;
    if (incoming.size() == 1) {
      mergedValue = incoming.front().first;
    } else {
      mergedValue = freshValueName(function, valueStem);
      std::vector<std::string> mergeOperands{instruction.operands().front()};
      for (const auto& [value, predecessor] : incoming) {
        mergeOperands.push_back(value);
        mergeOperands.push_back(predecessor);
      }
      mergeBlock.addInstruction(
          Instruction{"phi", std::move(mergeOperands), mergedValue});
    }
    std::vector<std::string> operands{instruction.operands().front()};
    for (std::size_t index = 1; index + 1 < instruction.operands().size();
         index += 2) {
      if (oldSet.find(instruction.operands()[index + 1]) == oldSet.end()) {
        operands.push_back(instruction.operands()[index]);
        operands.push_back(instruction.operands()[index + 1]);
      }
    }
    operands.push_back(mergedValue);
    operands.push_back(mergedPredecessor);
    rewritten.emplace_back("phi", std::move(operands), instruction.result());
  }
  destination.instructions() = std::move(rewritten);
  return true;
}

bool createMergedEdgeBlock(Function& function, std::size_t destinationIndex,
                           const std::vector<std::size_t>& predecessorIndices,
                           const std::string& stem, const std::string& valueStem) {
  if (predecessorIndices.empty()) return false;
  const std::string destinationName = function.blocks()[destinationIndex].name();
  const std::string mergedName = freshBlockName(function, stem);
  std::vector<std::string> predecessorNames;
  predecessorNames.reserve(predecessorIndices.size());
  for (const auto predecessor : predecessorIndices) {
    predecessorNames.push_back(function.blocks()[predecessor].name());
  }
  BasicBlock merged{mergedName};
  if (!mergePhiEdges(function, function.blocks()[destinationIndex],
                     predecessorNames, mergedName, merged, valueStem)) {
    return false;
  }
  for (const auto predecessor : predecessorIndices) {
    if (!redirectBranch(function.blocks()[predecessor], destinationName,
                        mergedName)) {
      return false;
    }
  }
  merged.addInstruction(Instruction{"br", {destinationName}});
  function.addBlock(std::move(merged));
  return true;
}

bool canonicalizeOneLoop(Function& function) {
  const LoopAnalysis analysis(function);
  for (const auto& loop : analysis.loops()) {
    if (!loop.preheader.has_value()) {
      return createMergedEdgeBlock(function, loop.header,
                                   loop.outsidePredecessors,
                                   "loop.preheader", "loop.preheader.value");
    }
    if (!loop.hasUniqueLatch) {
      return createMergedEdgeBlock(function, loop.header, loop.latches,
                                   "loop.latch", "loop.latch.value");
    }
    if (!loop.hasDedicatedExits) {
      for (const auto& [source, destination] : loop.exitEdges) {
        bool hasOutsidePredecessor = false;
        for (const auto predecessor : analysis.predecessors()[destination]) {
          hasOutsidePredecessor |= !loop.contains(predecessor);
        }
        if (!hasOutsidePredecessor) continue;
        return createMergedEdgeBlock(function, destination, {source},
                                     "loop.exit", "loop.exit.value");
      }
    }
  }
  return false;
}

}  // namespace

void LoopCanonicalizationPass::run(compiler::ir::core::Module& module) {
  constexpr std::size_t kMaximumChangesPerFunction = 64;
  for (auto& function : module.functions()) {
    for (std::size_t change = 0; change < kMaximumChangesPerFunction; ++change) {
      if (!canonicalizeOneLoop(function)) break;
    }
  }
}

}  // namespace compiler::midend::pass
