#include "../../../include/compiler/midend/pass/FunctionInliningPass.h"
#include "../../../include/compiler/midend/pass/LateFunctionInliningPass.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

enum class InlineMode { Early, Late };
enum class Purity { Pure, ReadOnly, Impure };

struct FunctionAnalysis {
  std::size_t instructionCount = 0;
  std::size_t callCount = 0;
  Purity purity = Purity::Pure;
  bool recursive = false;
  bool hasPhi = false;
};

struct InlineCandidate {
  const Function *function = nullptr;
  FunctionAnalysis analysis;
};

bool isValueName(const std::string &value) {
  return !value.empty() && value.front() == '%';
}

bool isInternalCalleeName(const std::string &operand) {
  return !operand.empty() && operand.front() == '@';
}

std::string calleeNameFromOperand(const std::string &operand) {
  return isInternalCalleeName(operand) ? operand.substr(1) : operand;
}

std::vector<std::string> branchTargets(const Instruction &instruction) {
  if (instruction.opcode() != "br") {
    return {};
  }
  const auto &operands = instruction.operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

std::unordered_set<std::string> collectFunctionNames(const Module &module) {
  std::unordered_set<std::string> names;
  for (const auto &function : module.functions()) {
    names.insert(function.name());
  }
  return names;
}

std::unordered_map<std::string, std::vector<std::string>>
buildCallGraph(const Module &module) {
  const auto functionNames = collectFunctionNames(module);
  std::unordered_map<std::string, std::vector<std::string>> graph;
  for (const auto &function : module.functions()) {
    auto &callees = graph[function.name()];
    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }
        const auto callee =
            calleeNameFromOperand(instruction.operands().front());
        if (functionNames.find(callee) != functionNames.end()) {
          callees.push_back(callee);
        }
      }
    }
  }
  return graph;
}

std::unordered_set<std::string>
collectRecursiveFunctions(const Module &module) {
  const auto graph = buildCallGraph(module);
  std::unordered_set<std::string> recursive;
  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack;

  std::function<void(const std::string &)> dfs =
      [&](const std::string &function) {
        if (visiting.find(function) != visiting.end()) {
          const auto cycleStart =
              std::find(stack.begin(), stack.end(), function);
          for (auto it = cycleStart; it != stack.end(); ++it) {
            recursive.insert(*it);
          }
          return;
        }
        if (visited.find(function) != visited.end()) {
          return;
        }

        visiting.insert(function);
        stack.push_back(function);
        if (const auto found = graph.find(function); found != graph.end()) {
          for (const auto &callee : found->second) {
            dfs(callee);
          }
        }
        stack.pop_back();
        visiting.erase(function);
        visited.insert(function);
      };

  for (const auto &function : module.functions()) {
    dfs(function.name());
  }
  return recursive;
}

std::unordered_map<std::string, std::size_t>
collectCallCounts(const Module &module) {
  std::unordered_map<std::string, std::size_t> counts;
  for (const auto &function : module.functions()) {
    counts.emplace(function.name(), 0);
  }
  for (const auto &function : module.functions()) {
    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }
        const auto callee =
            calleeNameFromOperand(instruction.operands().front());
        if (auto found = counts.find(callee); found != counts.end()) {
          ++found->second;
        }
      }
    }
  }
  return counts;
}

Purity mergePurity(Purity lhs, Purity rhs) {
  return static_cast<int>(lhs) > static_cast<int>(rhs) ? lhs : rhs;
}

std::unordered_map<std::string, Purity> analyzePurity(const Module &module) {
  std::unordered_map<std::string, Purity> purity;
  for (const auto &function : module.functions()) {
    purity.emplace(function.name(), Purity::Pure);
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &function : module.functions()) {
      Purity next = Purity::Pure;
      for (const auto &block : function.blocks()) {
        for (const auto &instruction : block.instructions()) {
          const auto &opcode = instruction.opcode();
          if (opcode == "store") {
            next = mergePurity(next, Purity::Impure);
          } else if (opcode == "load") {
            next = mergePurity(next, Purity::ReadOnly);
          } else if (opcode == "call") {
            if (instruction.operands().empty()) {
              next = mergePurity(next, Purity::Impure);
              continue;
            }
            const auto callee =
                calleeNameFromOperand(instruction.operands().front());
            if (const auto found = purity.find(callee); found != purity.end()) {
              next = mergePurity(next, found->second);
            } else {
              next = mergePurity(next, Purity::Impure);
            }
          }
        }
      }
      if (purity[function.name()] != next) {
        purity[function.name()] = next;
        changed = true;
      }
    }
  }
  return purity;
}

std::unordered_map<std::string, FunctionAnalysis>
analyzeFunctions(const Module &module) {
  const auto recursive = collectRecursiveFunctions(module);
  const auto callCounts = collectCallCounts(module);
  const auto purity = analyzePurity(module);
  std::unordered_map<std::string, FunctionAnalysis> analyses;

  for (const auto &function : module.functions()) {
    FunctionAnalysis analysis;
    analysis.callCount = callCounts.find(function.name()) == callCounts.end()
                             ? 0
                             : callCounts.find(function.name())->second;
    analysis.purity = purity.find(function.name()) == purity.end()
                          ? Purity::Impure
                          : purity.find(function.name())->second;
    analysis.recursive = recursive.find(function.name()) != recursive.end();
    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() == "phi") {
          analysis.hasPhi = true;
        }
        if (instruction.opcode() != "ret") {
          ++analysis.instructionCount;
        }
      }
    }
    analyses.emplace(function.name(), analysis);
  }
  return analyses;
}

bool isInlineCandidate(const Function &function,
                       const FunctionAnalysis &analysis, InlineMode mode) {
  if (function.name() == "main" || analysis.recursive ||
      function.blocks().empty()) {
    return false;
  }

  if (mode == InlineMode::Early) {
    if (analysis.purity == Purity::Impure) {
      return false;
    }
    const std::size_t limit = analysis.callCount <= 1 ? 24 : 12;
    return analysis.instructionCount <= limit;
  }

  const std::size_t limit = analysis.callCount <= 1 ? 80 : 36;
  return analysis.instructionCount <= limit;
}

std::unordered_map<std::string, InlineCandidate>
collectInlineCandidates(const Module &module, InlineMode mode) {
  const auto analyses = analyzeFunctions(module);
  std::unordered_map<std::string, InlineCandidate> candidates;
  for (const auto &function : module.functions()) {
    const auto analysis = analyses.find(function.name());
    if (analysis == analyses.end() ||
        !isInlineCandidate(function, analysis->second, mode)) {
      continue;
    }
    candidates.emplace(function.name(),
                       InlineCandidate{&function, analysis->second});
  }
  return candidates;
}

std::unordered_set<std::string>
collectUsedValueNames(const Function &function) {
  std::unordered_set<std::string> used;
  for (const auto &parameter : function.parameters()) {
    used.insert(parameter.name);
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }
  return used;
}

std::unordered_set<std::string>
collectUsedBlockNames(const Function &function) {
  std::unordered_set<std::string> used;
  for (const auto &block : function.blocks()) {
    used.insert(block.name());
  }
  return used;
}

std::string freshValueName(std::unordered_set<std::string> &used,
                           const std::string &original) {
  std::string stem = "%inl";
  if (isValueName(original)) {
    stem += original.substr(1);
  }

  for (std::size_t index = 0;; ++index) {
    const std::string candidate = stem + "." + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string freshBlockName(std::unordered_set<std::string> &used,
                           const std::string &stem) {
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = stem + "." + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string
rewriteOperand(const std::string &value,
               const std::unordered_map<std::string, std::string> &valueMap,
               const std::unordered_map<std::string, std::string> &blockMap) {
  if (const auto found = valueMap.find(value); found != valueMap.end()) {
    return found->second;
  }
  if (const auto found = blockMap.find(value); found != blockMap.end()) {
    return found->second;
  }
  return value;
}

std::vector<std::string>
rewriteOperands(const std::vector<std::string> &operands,
                const std::unordered_map<std::string, std::string> &valueMap,
                const std::unordered_map<std::string, std::string> &blockMap) {
  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());
  for (const auto &operand : operands) {
    rewritten.push_back(rewriteOperand(operand, valueMap, blockMap));
  }
  return rewritten;
}

struct ReturnIncoming {
  std::string block;
  std::string value;
};

void prepareCloneNames(const Function &callee, Function &caller,
                       std::unordered_set<std::string> &usedValues,
                       std::unordered_set<std::string> &usedBlocks,
                       std::unordered_map<std::string, std::string> &valueMap,
                       std::unordered_map<std::string, std::string> &blockMap,
                       const std::string &callBlockName) {
  for (const auto &block : callee.blocks()) {
    blockMap.emplace(
        block.name(),
        freshBlockName(usedBlocks, callBlockName + ".inl." + block.name()));
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        valueMap.emplace(instruction.result(),
                         freshValueName(usedValues, instruction.result()));
      }
    }
  }
  (void)caller;
}

bool inlineCallAt(Function &caller, std::size_t blockIndex,
                  std::size_t instructionIndex,
                  const InlineCandidate &candidate) {
  const auto &callee = *candidate.function;
  const auto &callInstruction =
      caller.blocks()[blockIndex].instructions()[instructionIndex];
  const auto callOperands = callInstruction.operands();
  if (callOperands.size() - 1 != callee.parameters().size()) {
    return false;
  }
  if (callee.returnType() == "void" && callInstruction.hasResult()) {
    return false;
  }
  if (callee.returnType() != "void" && callInstruction.hasResult() &&
      callee.blocks().empty()) {
    return false;
  }

  auto usedValues = collectUsedValueNames(caller);
  auto usedBlocks = collectUsedBlockNames(caller);
  std::unordered_map<std::string, std::string> valueMap;
  std::unordered_map<std::string, std::string> blockMap;

  for (std::size_t i = 0; i < callee.parameters().size(); ++i) {
    valueMap.emplace(callee.parameters()[i].name, callOperands[i + 1]);
  }

  const std::string originalBlockName = caller.blocks()[blockIndex].name();
  const std::string continuationName =
      freshBlockName(usedBlocks, originalBlockName + ".inl.cont");
  prepareCloneNames(callee, caller, usedValues, usedBlocks, valueMap, blockMap,
                    originalBlockName);

  std::vector<Instruction> prefix;
  std::vector<Instruction> suffix;
  const auto &originalInstructions = caller.blocks()[blockIndex].instructions();
  for (std::size_t i = 0; i < instructionIndex; ++i) {
    prefix.push_back(originalInstructions[i]);
  }
  for (std::size_t i = instructionIndex + 1; i < originalInstructions.size();
       ++i) {
    suffix.push_back(originalInstructions[i]);
  }
  std::unordered_set<std::string> originalSuccessors;
  if (!suffix.empty()) {
    for (const auto &target : branchTargets(suffix.back())) {
      originalSuccessors.insert(target);
    }
  }
  prefix.emplace_back(
      "br", std::vector<std::string>{blockMap[callee.blocks().front().name()]});

  std::vector<BasicBlock> clonedBlocks;
  std::vector<ReturnIncoming> returnIncoming;
  clonedBlocks.reserve(callee.blocks().size());
  for (const auto &calleeBlock : callee.blocks()) {
    BasicBlock cloned{blockMap[calleeBlock.name()]};
    for (const auto &instruction : calleeBlock.instructions()) {
      if (instruction.opcode() == "ret") {
        if (!instruction.operands().empty()) {
          returnIncoming.push_back(ReturnIncoming{
              cloned.name(), rewriteOperand(instruction.operands().front(),
                                            valueMap, blockMap)});
        }
        cloned.addInstruction(Instruction{"br", {continuationName}});
        continue;
      }
      cloned.addInstruction(Instruction{
          instruction.opcode(),
          rewriteOperands(instruction.operands(), valueMap, blockMap),
          instruction.hasResult() ? valueMap[instruction.result()]
                                  : std::string{}});
    }
    clonedBlocks.push_back(std::move(cloned));
  }

  BasicBlock continuation{continuationName};
  if (callee.returnType() != "void" && callInstruction.hasResult() &&
      !returnIncoming.empty()) {
    std::vector<std::string> operands;
    operands.push_back(callee.returnType());
    for (const auto &incoming : returnIncoming) {
      operands.push_back(incoming.value);
      operands.push_back(incoming.block);
    }
    continuation.addInstruction(
        Instruction{"phi", std::move(operands), callInstruction.result()});
  }
  for (const auto &instruction : suffix) {
    continuation.addInstruction(instruction);
  }

  std::deque<BasicBlock> rewrittenBlocks;
  for (std::size_t i = 0; i < caller.blocks().size(); ++i) {
    if (i != blockIndex) {
      rewrittenBlocks.push_back(caller.blocks()[i]);
      continue;
    }

    BasicBlock rewrittenOriginal{originalBlockName};
    for (const auto &instruction : prefix) {
      rewrittenOriginal.addInstruction(instruction);
    }
    rewrittenBlocks.push_back(std::move(rewrittenOriginal));
    for (auto &cloned : clonedBlocks) {
      rewrittenBlocks.push_back(std::move(cloned));
    }
    rewrittenBlocks.push_back(std::move(continuation));
  }

  for (auto &block : rewrittenBlocks) {
    if (originalSuccessors.find(block.name()) == originalSuccessors.end()) {
      continue;
    }
    for (auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        break;
      }
      auto operands = instruction.operands();
      for (std::size_t i = 2; i < operands.size(); i += 2) {
        if (operands[i] == originalBlockName) {
          operands[i] = continuationName;
        }
      }
      instruction =
          Instruction{"phi", std::move(operands), instruction.result()};
    }
  }

  caller.blocks() = std::move(rewrittenBlocks);
  return true;
}

bool inlineOneCall(
    Function &function,
    const std::unordered_map<std::string, InlineCandidate> &candidates) {
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto &instructions = function.blocks()[blockIndex].instructions();
    for (std::size_t instructionIndex = 0;
         instructionIndex < instructions.size(); ++instructionIndex) {
      const auto &instruction = instructions[instructionIndex];
      if (instruction.opcode() != "call" || instruction.operands().empty()) {
        continue;
      }

      const auto callee = calleeNameFromOperand(instruction.operands().front());
      const auto candidate = candidates.find(callee);
      if (candidate == candidates.end() ||
          candidate->second.function->name() == function.name()) {
        continue;
      }
      return inlineCallAt(function, blockIndex, instructionIndex,
                          candidate->second);
    }
  }
  return false;
}

bool runInlining(Module &module, InlineMode mode) {
  bool changed = false;
  constexpr std::size_t maxInlineSitesPerPass = 96;
  std::size_t inlineSites = 0;

  while (inlineSites < maxInlineSitesPerPass) {
    const auto candidates = collectInlineCandidates(module, mode);
    if (candidates.empty()) {
      break;
    }

    bool inlinedThisRound = false;
    for (auto &function : module.functions()) {
      if (inlineSites >= maxInlineSitesPerPass) {
        break;
      }
      if (inlineOneCall(function, candidates)) {
        ++inlineSites;
        changed = true;
        inlinedThisRound = true;
      }
    }
    if (!inlinedThisRound) {
      break;
    }
  }

  return changed;
}

} // namespace

bool FunctionInliningPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  runInlining(module, InlineMode::Early);
  return before != detail::moduleFingerprint(module);
}

bool LateFunctionInliningPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  runInlining(module, InlineMode::Late);
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
