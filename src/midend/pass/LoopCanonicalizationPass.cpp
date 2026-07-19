#include "../../../include/compiler/midend/pass/LoopCanonicalizationPass.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/analysis/LoopAnalysis.h"
#include "../../../include/compiler/ir/analysis/UseDefAnalysis.h"
#include "../../../include/compiler/ir/core/BasicBlock.h"
#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Instruction.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::analysis::LoopAnalysis;
using compiler::ir::analysis::NaturalLoop;
using compiler::ir::analysis::UseDefAnalysis;
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
  std::unordered_set<std::string> valueNames;
  for (const auto& parameter : function.parameters()) {
    valueNames.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) valueNames.insert(instruction.result());
    }
  }
  const auto nextValueName = [&]() {
    std::string result = "%" + valueStem;
    for (std::size_t suffix = 0; !valueNames.insert(result).second; ++suffix) {
      result = "%" + valueStem + "." + std::to_string(suffix + 1);
    }
    return result;
  };
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
      mergedValue = nextValueName();
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

bool hasOption(LoopCanonicalizationOption options,
               LoopCanonicalizationOption option) {
  return (static_cast<unsigned>(options) & static_cast<unsigned>(option)) != 0;
}

bool canonicalizeOneLoop(Function& function,
                         LoopCanonicalizationOption options) {
  const LoopAnalysis analysis(function);
  for (const auto& loop : analysis.loops()) {
    if (hasOption(options, LoopCanonicalizationOption::Preheaders) &&
        !loop.preheader.has_value()) {
      return createMergedEdgeBlock(function, loop.header,
                                   loop.outsidePredecessors,
                                   "loop.preheader", "loop.preheader.value");
    }
    if (hasOption(options, LoopCanonicalizationOption::Latches) &&
        !loop.hasUniqueLatch) {
      return createMergedEdgeBlock(function, loop.header, loop.latches,
                                   "loop.latch", "loop.latch.value");
    }
    if (hasOption(options, LoopCanonicalizationOption::DedicatedExits) &&
        !loop.hasDedicatedExits) {
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

bool formSingleExitLoopClosedSSA(const compiler::ir::core::Module& module,
                                 Function& function,
                                 const NaturalLoop& loop,
                                 const LoopAnalysis& analysis) {
  if (!loop.hasDedicatedExits || loop.isLoopClosedSSA) return false;
  std::unordered_set<std::size_t> exitTargets;
  for (const auto& edge : loop.exitEdges) exitTargets.insert(edge.second);
  if (exitTargets.size() != 1) return false;
  const std::size_t exitTarget = *exitTargets.begin();

  const UseDefAnalysis useDef(module, function);
  auto valueNames = std::unordered_set<std::string>{};
  for (const auto& parameter : function.parameters()) {
    valueNames.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) valueNames.insert(instruction.result());
    }
  }

  std::unordered_map<std::string, std::string> replacements;
  std::vector<Instruction> exitPhis;
  std::unordered_set<std::size_t> exitSources;
  for (const auto& [source, target] : loop.exitEdges) {
    if (target == exitTarget) exitSources.insert(source);
  }
  for (const auto block : loop.blocks) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (!instruction.hasResult()) continue;
      const auto& value = instruction.result();
      bool hasNonLcssaOutsideUse = false;
      for (const auto& use : useDef.uses(value)) {
        if (loop.contains(use.blockIndex)) continue;
        const auto& user = function.blocks()[use.blockIndex]
                               .instructions()[use.instructionIndex];
        const bool existingExitPhi =
            use.blockIndex == exitTarget && user.opcode() == "phi" &&
            use.operandIndex % 2 == 1;
        hasNonLcssaOutsideUse |= !existingExitPhi;
      }
      if (!hasNonLcssaOutsideUse) continue;
      bool availableOnEveryExit = true;
      for (const auto source : exitSources) {
        availableOnEveryExit &= analysis.dominates(block, source);
      }
      if (!availableOnEveryExit) continue;
      const auto type = useDef.typeOf(value);
      if (type.empty() || type == "void" || type.rfind("ptr(", 0) == 0) {
        continue;
      }
      std::string lcssaValue = "%lcssa.exit.value";
      for (std::size_t suffix = 0; !valueNames.insert(lcssaValue).second;
           ++suffix) {
        lcssaValue = "%lcssa.exit.value." + std::to_string(suffix + 1);
      }
      std::vector<std::string> operands{type};
      std::vector<std::size_t> orderedSources(exitSources.begin(),
                                              exitSources.end());
      std::sort(orderedSources.begin(), orderedSources.end());
      for (const auto source : orderedSources) {
        operands.push_back(value);
        operands.push_back(function.blocks()[source].name());
      }
      exitPhis.emplace_back("phi", std::move(operands), lcssaValue);
      replacements.emplace(value, lcssaValue);
    }
  }
  if (exitPhis.empty()) return false;

  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (loop.contains(block)) continue;
    for (auto& instruction : function.blocks()[block].instructions()) {
      auto operands = instruction.operands();
      bool changed = false;
      for (std::size_t index = 0; index < operands.size(); ++index) {
        const auto replacement = replacements.find(operands[index]);
        if (replacement == replacements.end()) continue;
        const bool preserveExitIncoming =
            block == exitTarget && instruction.opcode() == "phi" &&
            index % 2 == 1;
        if (preserveExitIncoming) continue;
        operands[index] = replacement->second;
        changed = true;
      }
      if (changed) {
        instruction = Instruction{instruction.opcode(), std::move(operands),
                                  instruction.result()};
      }
    }
  }
  auto& exitInstructions = function.blocks()[exitTarget].instructions();
  exitInstructions.insert(exitInstructions.begin(), exitPhis.begin(),
                          exitPhis.end());
  return true;
}

}  // namespace

bool ensureLoopPreheader(Function& function, std::size_t headerIndex) {
  const LoopAnalysis analysis(function);
  for (const auto& loop : analysis.loops()) {
    if (loop.header != headerIndex) continue;
    if (loop.preheader.has_value()) return false;
    return createMergedEdgeBlock(function, loop.header,
                                 loop.outsidePredecessors,
                                 "loop.preheader", "loop.preheader.value");
  }
  return false;
}

bool formLoopClosedSSA(const compiler::ir::core::Module& module,
                       Function& function, std::size_t headerIndex) {
  const LoopAnalysis analysis(function);
  for (const auto& loop : analysis.loops()) {
    if (loop.header == headerIndex) {
      return formSingleExitLoopClosedSSA(module, function, loop, analysis);
    }
  }
  return false;
}

void LoopCanonicalizationPass::run(compiler::ir::core::Module& module) {
  constexpr std::size_t kMaximumChangesPerFunction = 64;
  for (auto& function : module.functions()) {
    for (std::size_t change = 0; change < kMaximumChangesPerFunction; ++change) {
      if (!canonicalizeOneLoop(function, options_)) break;
    }
    if (hasOption(options_, LoopCanonicalizationOption::LoopClosedSSA)) {
      for (std::size_t change = 0; change < kMaximumChangesPerFunction;
           ++change) {
        const LoopAnalysis analysis(function);
        bool changed = false;
        for (const auto& loop : analysis.loops()) {
          if (formSingleExitLoopClosedSSA(module, function, loop, analysis)) {
            changed = true;
            break;
          }
        }
        if (!changed) break;
      }
    }
  }
}

}  // namespace compiler::midend::pass
