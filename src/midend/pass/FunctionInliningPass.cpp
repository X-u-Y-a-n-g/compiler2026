#include "../../../include/compiler/midend/pass/FunctionInliningPass.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

struct InlineCandidate {
  std::string name;
  std::vector<std::string> parameters;
  std::vector<Instruction> instructions;
  std::string returnValue;
  bool isConditionalReturn = false;
  std::string condition;
  std::string trueReturnValue;
  std::string falseReturnValue;
};

bool isValueName(const std::string& value) {
  return !value.empty() && value.front() == '%';
}

bool isPureInlineInstruction(const Instruction& instruction) {
  if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
    return false;
  }

  const auto& opcode = instruction.opcode();
  return opcode != "alloca" && opcode != "load" && opcode != "phi" && opcode != "call";
}

std::unordered_set<std::string> collectRecursiveFunctions(const Module& module) {
  std::unordered_set<std::string> recursive;
  for (const auto& function : module.functions()) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() == "call" && !instruction.operands().empty() &&
            instruction.operands().front() == "@" + function.name()) {
          recursive.insert(function.name());
        }
      }
    }
  }
  return recursive;
}

std::unordered_map<std::string, InlineCandidate> collectInlineCandidates(const Module& module) {
  constexpr std::size_t kMaxInlineInstructions = 16;
  std::unordered_map<std::string, InlineCandidate> candidates;
  const auto recursive = collectRecursiveFunctions(module);

  for (const auto& function : module.functions()) {
    if (function.name() == "main" || function.returnType() == "void" ||
        recursive.find(function.name()) != recursive.end()) {
      continue;
    }

    InlineCandidate candidate;
    candidate.name = function.name();
    for (const auto& parameter : function.parameters()) {
      candidate.parameters.push_back(parameter.name);
    }

    if (function.blocks().size() == 1) {
      const auto& instructions = function.blocks().front().instructions();
      if (instructions.empty()) {
        continue;
      }

      const auto& terminator = instructions.back();
      if (terminator.opcode() != "ret" || terminator.operands().size() != 1 ||
          instructions.size() - 1 > kMaxInlineInstructions) {
        continue;
      }
      candidate.returnValue = terminator.operands().front();

      bool supported = true;
      for (std::size_t i = 0; i + 1 < instructions.size(); ++i) {
        if (!isPureInlineInstruction(instructions[i])) {
          supported = false;
          break;
        }
        candidate.instructions.push_back(instructions[i]);
      }
      if (supported) {
        candidates.emplace(function.name(), std::move(candidate));
      }
      continue;
    }

    if (function.returnType() != "int" || function.blocks().size() < 3) {
      continue;
    }

    const auto& entryInstructions = function.blocks().front().instructions();
    if (entryInstructions.empty() || entryInstructions.size() - 1 > kMaxInlineInstructions) {
      continue;
    }
    const auto& terminator = entryInstructions.back();
    if (terminator.opcode() != "br" || terminator.operands().size() != 3) {
      continue;
    }

    const auto findBlock = [&](const std::string& name) -> const compiler::ir::core::BasicBlock* {
      for (const auto& block : function.blocks()) {
        if (block.name() == name) {
          return &block;
        }
      }
      return nullptr;
    };
    const auto* trueBlock = findBlock(terminator.operands()[1]);
    const auto* falseBlock = findBlock(terminator.operands()[2]);
    if (trueBlock == nullptr || falseBlock == nullptr || trueBlock == falseBlock ||
        trueBlock->instructions().size() != 1 || falseBlock->instructions().size() != 1) {
      continue;
    }
    const auto& trueReturn = trueBlock->instructions().front();
    const auto& falseReturn = falseBlock->instructions().front();
    if (trueReturn.opcode() != "ret" || trueReturn.operands().size() != 1 ||
        falseReturn.opcode() != "ret" || falseReturn.operands().size() != 1) {
      continue;
    }

    bool supported = true;
    for (std::size_t i = 0; i + 1 < entryInstructions.size(); ++i) {
      if (!isPureInlineInstruction(entryInstructions[i])) {
        supported = false;
        break;
      }
      candidate.instructions.push_back(entryInstructions[i]);
    }
    if (!supported) {
      continue;
    }

    candidate.isConditionalReturn = true;
    candidate.condition = terminator.operands()[0];
    candidate.trueReturnValue = trueReturn.operands()[0];
    candidate.falseReturnValue = falseReturn.operands()[0];
    candidates.emplace(function.name(), std::move(candidate));
  }

  return candidates;
}

std::unordered_set<std::string> collectUsedValueNames(const Function& function) {
  std::unordered_set<std::string> used;
  for (const auto& parameter : function.parameters()) {
    used.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }
  return used;
}

std::string freshValueName(std::unordered_set<std::string>& used, const std::string& original) {
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

std::string rewriteValue(const std::string& value,
                         const std::unordered_map<std::string, std::string>& replacements) {
  const auto found = replacements.find(value);
  if (found != replacements.end()) {
    return found->second;
  }
  return detail::resolveAlias(value, replacements);
}

std::vector<std::string> rewriteInlineOperands(
    const std::vector<std::string>& operands,
    const std::unordered_map<std::string, std::string>& replacements) {
  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());
  for (const auto& operand : operands) {
    rewritten.push_back(rewriteValue(operand, replacements));
  }
  return rewritten;
}

void rewriteFunctionOperands(Function& function,
                             const std::unordered_map<std::string, std::string>& aliases) {
  if (aliases.empty()) {
    return;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(), aliases),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

bool inlineFunctionCalls(
    Function& function, const std::unordered_map<std::string, InlineCandidate>& candidates) {
  bool changed = false;
  auto usedNames = collectUsedValueNames(function);
  std::unordered_map<std::string, std::string> aliases;

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      if (instruction.opcode() != "call" || operands.empty()) {
        rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
        continue;
      }

      const std::string callee =
          !operands.front().empty() && operands.front().front() == '@'
              ? operands.front().substr(1)
              : operands.front();
      const auto candidateIt = candidates.find(callee);
      if (candidateIt == candidates.end() || candidateIt->second.name == function.name() ||
          operands.size() - 1 != candidateIt->second.parameters.size()) {
        rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
        continue;
      }

      const auto& candidate = candidateIt->second;
      std::unordered_map<std::string, std::string> replacements;
      for (std::size_t i = 0; i < candidate.parameters.size(); ++i) {
        replacements.emplace(candidate.parameters[i], operands[i + 1]);
      }

      for (const auto& inlinedInstruction : candidate.instructions) {
        const auto clonedResult = freshValueName(usedNames, inlinedInstruction.result());
        replacements.emplace(inlinedInstruction.result(), clonedResult);
        rewritten.emplace_back(inlinedInstruction.opcode(),
                               rewriteInlineOperands(inlinedInstruction.operands(), replacements),
                               clonedResult);
      }

      if (candidate.isConditionalReturn) {
        if (instruction.hasResult()) {
          rewritten.emplace_back(
              "select",
              std::vector<std::string>{rewriteValue(candidate.condition, replacements),
                                       rewriteValue(candidate.trueReturnValue, replacements),
                                       rewriteValue(candidate.falseReturnValue, replacements)},
              instruction.result());
        }
      } else if (instruction.hasResult()) {
        aliases[instruction.result()] = rewriteValue(candidate.returnValue, replacements);
      }
      changed = true;
    }

    block.instructions() = std::move(rewritten);
  }

  rewriteFunctionOperands(function, aliases);
  return changed;
}

std::size_t instructionCount(const Function& function) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) {
    count += block.instructions().size();
  }
  return count;
}

std::unordered_set<std::string> collectBlockNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshBlockName(std::unordered_set<std::string>& names,
                           const std::string& original) {
  const std::string stem = "inl." + original;
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::vector<std::string> branchSuccessors(const Instruction& instruction) {
  if (instruction.opcode() != "br") {
    return {};
  }
  if (instruction.operands().size() == 1) {
    return {instruction.operands().front()};
  }
  if (instruction.operands().size() == 3) {
    return {instruction.operands()[1], instruction.operands()[2]};
  }
  return {};
}

std::unordered_map<std::string, std::vector<std::string>> buildSuccessors(
    const Function& function) {
  std::unordered_map<std::string, std::vector<std::string>> successors;
  for (const auto& block : function.blocks()) {
    auto& targets = successors[block.name()];
    if (!block.instructions().empty()) {
      targets = branchSuccessors(block.instructions().back());
    }
  }
  return successors;
}

bool reachesBlock(
    const std::unordered_map<std::string, std::vector<std::string>>& successors,
    const std::string& start, const std::string& target) {
  std::deque<std::string> worklist{start};
  std::unordered_set<std::string> visited;
  while (!worklist.empty()) {
    auto current = std::move(worklist.front());
    worklist.pop_front();
    if (current == target) {
      return true;
    }
    if (!visited.insert(current).second) {
      continue;
    }
    const auto found = successors.find(current);
    if (found != successors.end()) {
      for (const auto& successor : found->second) {
        worklist.push_back(successor);
      }
    }
  }
  return false;
}

std::unordered_set<std::string> cyclicBlocks(const Function& function) {
  const auto successors = buildSuccessors(function);
  std::unordered_set<std::string> cyclic;
  for (const auto& block : function.blocks()) {
    const auto found = successors.find(block.name());
    if (found == successors.end()) {
      continue;
    }
    for (const auto& successor : found->second) {
      if (reachesBlock(successors, successor, block.name())) {
        cyclic.insert(block.name());
        break;
      }
    }
  }
  return cyclic;
}

std::unordered_map<std::string, std::vector<std::string>> buildCallGraph(
    const Module& module) {
  std::unordered_map<std::string, std::vector<std::string>> calls;
  for (const auto& function : module.functions()) {
    auto& callees = calls[function.name()];
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }
        const auto& operand = instruction.operands().front();
        callees.push_back(!operand.empty() && operand.front() == '@'
                              ? operand.substr(1)
                              : operand);
      }
    }
  }
  return calls;
}

bool callGraphReaches(
    const std::unordered_map<std::string, std::vector<std::string>>& calls,
    const std::string& start, const std::string& target) {
  std::deque<std::string> worklist{start};
  std::unordered_set<std::string> visited;
  while (!worklist.empty()) {
    auto current = std::move(worklist.front());
    worklist.pop_front();
    if (current == target) {
      return true;
    }
    if (!visited.insert(current).second) {
      continue;
    }
    const auto found = calls.find(current);
    if (found != calls.end()) {
      for (const auto& callee : found->second) {
        worklist.push_back(callee);
      }
    }
  }
  return false;
}

bool isGeneralInlineCandidate(const Function& function) {
  if (function.name() == "main" || function.returnType() == "void" ||
      function.blocks().empty()) {
    return false;
  }
  bool sawReturn = false;
  for (const auto& block : function.blocks()) {
    if (block.instructions().empty()) {
      return false;
    }
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "ret") {
        continue;
      }
      const std::size_t expected = function.returnType() == "void" ? 0 : 1;
      if (instruction.operands().size() != expected) {
        return false;
      }
      sawReturn = true;
    }
  }
  return sawReturn;
}

std::vector<std::string> rewriteGeneralOperands(
    const Instruction& instruction,
    const std::unordered_map<std::string, std::string>& values,
    const std::unordered_map<std::string, std::string>& blocks) {
  const auto rewriteValueName = [&](const std::string& value) {
    const auto found = values.find(value);
    return found == values.end() ? value : found->second;
  };
  const auto rewriteBlockName = [&](const std::string& block) {
    const auto found = blocks.find(block);
    return found == blocks.end() ? block : found->second;
  };

  const auto& operands = instruction.operands();
  if (instruction.opcode() == "br") {
    if (operands.size() == 1) {
      return {rewriteBlockName(operands.front())};
    }
    if (operands.size() == 3) {
      return {rewriteValueName(operands[0]), rewriteBlockName(operands[1]),
              rewriteBlockName(operands[2])};
    }
  }
  if (instruction.opcode() == "phi" && !operands.empty()) {
    std::vector<std::string> rewritten{operands.front()};
    for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
      rewritten.push_back(rewriteValueName(operands[index]));
      rewritten.push_back(rewriteBlockName(operands[index + 1]));
    }
    return rewritten;
  }

  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());
  for (const auto& operand : operands) {
    rewritten.push_back(rewriteValueName(operand));
  }
  return rewritten;
}

void replacePhiPredecessor(Function& function,
                           const std::vector<std::string>& successors,
                           const std::string& oldPredecessor,
                           const std::string& newPredecessor) {
  const std::unordered_set<std::string> successorSet(successors.begin(),
                                                     successors.end());
  for (auto& block : function.blocks()) {
    if (successorSet.find(block.name()) == successorSet.end()) {
      continue;
    }
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        continue;
      }
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldPredecessor) {
          operands[index] = newPredecessor;
        }
      }
      instruction = Instruction{"phi", std::move(operands),
                                instruction.result()};
    }
  }
}

bool inlineGeneralCallSite(Function& caller, const Function& callee,
                           std::size_t blockIndex, std::size_t callIndex) {
  const auto call = caller.blocks()[blockIndex].instructions()[callIndex];
  if (call.opcode() != "call" || call.operands().size() !=
                                      callee.parameters().size() + 1) {
    return false;
  }

  auto valueNames = collectUsedValueNames(caller);
  auto blockNames = collectBlockNames(caller);
  const std::string callerBlockName = caller.blocks()[blockIndex].name();
  const std::string continuationName =
      freshBlockName(blockNames, "continuation");

  std::unordered_map<std::string, std::string> blockReplacements;
  for (const auto& block : callee.blocks()) {
    blockReplacements.emplace(block.name(),
                              freshBlockName(blockNames, block.name()));
  }
  std::unordered_map<std::string, std::string> valueReplacements;
  for (std::size_t index = 0; index < callee.parameters().size(); ++index) {
    valueReplacements.emplace(callee.parameters()[index].name,
                              call.operands()[index + 1]);
  }
  for (const auto& block : callee.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        valueReplacements.emplace(
            instruction.result(),
            freshValueName(valueNames, instruction.result()));
      }
    }
  }

  std::vector<compiler::ir::core::BasicBlock> clonedBlocks;
  std::vector<std::pair<std::string, std::string>> returns;
  for (const auto& sourceBlock : callee.blocks()) {
    compiler::ir::core::BasicBlock cloned{
        blockReplacements.at(sourceBlock.name())};
    for (const auto& instruction : sourceBlock.instructions()) {
      if (instruction.opcode() == "ret") {
        if (!instruction.operands().empty()) {
          const auto found = valueReplacements.find(instruction.operands()[0]);
          returns.emplace_back(found == valueReplacements.end()
                                   ? instruction.operands()[0]
                                   : found->second,
                               cloned.name());
        }
        cloned.addInstruction(Instruction{"br", {continuationName}});
        continue;
      }
      const std::string result =
          instruction.hasResult()
              ? valueReplacements.at(instruction.result())
              : std::string{};
      cloned.addInstruction(Instruction{
          instruction.opcode(),
          rewriteGeneralOperands(instruction, valueReplacements,
                                 blockReplacements),
          result});
    }
    clonedBlocks.push_back(std::move(cloned));
  }

  auto& instructions = caller.blocks()[blockIndex].instructions();
  std::vector<Instruction> suffix(
      instructions.begin() + static_cast<std::ptrdiff_t>(callIndex + 1),
      instructions.end());
  std::vector<std::string> oldSuccessors;
  if (!suffix.empty()) {
    oldSuccessors = branchSuccessors(suffix.back());
  }
  instructions.erase(instructions.begin() +
                         static_cast<std::ptrdiff_t>(callIndex),
                     instructions.end());
  instructions.emplace_back(
      "br", std::vector<std::string>{
                blockReplacements.at(callee.blocks().front().name())});
  replacePhiPredecessor(caller, oldSuccessors, callerBlockName,
                        continuationName);

  auto insertionPoint =
      caller.blocks().begin() + static_cast<std::ptrdiff_t>(blockIndex + 1);
  for (auto& block : clonedBlocks) {
    insertionPoint = caller.blocks().insert(insertionPoint, std::move(block));
    ++insertionPoint;
  }
  compiler::ir::core::BasicBlock continuation{continuationName};
  if (callee.returnType() != "void" && call.hasResult()) {
    std::vector<std::string> phiOperands{callee.returnType()};
    for (const auto& [value, predecessor] : returns) {
      phiOperands.push_back(value);
      phiOperands.push_back(predecessor);
    }
    continuation.addInstruction(
        Instruction{"phi", std::move(phiOperands), call.result()});
  }
  for (auto& instruction : suffix) {
    continuation.addInstruction(std::move(instruction));
  }
  caller.blocks().insert(insertionPoint, std::move(continuation));
  return true;
}

std::size_t staticCallCount(
    const std::unordered_map<std::string, std::vector<std::string>>& callGraph,
    const std::string& callee) {
  std::size_t count = 0;
  for (const auto& [caller, callees] : callGraph) {
    (void)caller;
    count += static_cast<std::size_t>(
        std::count(callees.begin(), callees.end(), callee));
  }
  return count;
}

bool inlineOneGeneralCall(Module& module, std::size_t& totalGrowth,
                          std::unordered_set<std::string>& inlinedCallees) {
  constexpr std::size_t kSmallCalleeLimit = 24;
  // A single-call loop is removed after cloning, so a slightly larger local
  // limit still reduces whole-module code size while eliminating a hot call.
  constexpr std::size_t kSmallLoopCalleeLimit = 48;
  constexpr std::size_t kCallerGrowthLimit = 160;
  constexpr std::size_t kModuleGrowthLimit = 320;

  const auto callGraph = buildCallGraph(module);
  std::unordered_map<std::string, Function> snapshots;
  for (const auto& function : module.functions()) {
    snapshots.emplace(function.name(), function);
  }

  std::unordered_set<std::string> calledFunctions;
  for (const auto& [callerName, callees] : callGraph) {
    (void)callerName;
    calledFunctions.insert(callees.begin(), callees.end());
  }

  // Prefer call-graph roots first.  This keeps original callees compact while
  // exposing constants and control-flow context at outer hot call sites;
  // subsequent iterations may then inline helpers into the cloned body.
  for (int rootPass = 0; rootPass < 2; ++rootPass) {
    for (auto& caller : module.functions()) {
      const bool isRoot = calledFunctions.find(caller.name()) ==
                          calledFunctions.end();
      if ((rootPass == 0) != isRoot) {
        continue;
      }
      const auto hotBlocks = cyclicBlocks(caller);
      std::size_t callerGrowth = 0;
      for (std::size_t blockIndex = 0; blockIndex < caller.blocks().size();
           ++blockIndex) {
        const bool hot = hotBlocks.find(caller.blocks()[blockIndex].name()) !=
                         hotBlocks.end();
        for (std::size_t instructionIndex = 0;
             instructionIndex <
             caller.blocks()[blockIndex].instructions().size();
             ++instructionIndex) {
          const auto& call =
              caller.blocks()[blockIndex].instructions()[instructionIndex];
          if (call.opcode() != "call" || call.operands().empty()) {
            continue;
          }
          const auto& target = call.operands().front();
          const std::string calleeName =
              !target.empty() && target.front() == '@' ? target.substr(1) : target;
          const auto found = snapshots.find(calleeName);
          const bool calleeHasLoop =
              found != snapshots.end() && !cyclicBlocks(found->second).empty();
          if (!hot || found == snapshots.end() || calleeName == caller.name() ||
              staticCallCount(callGraph, calleeName) != 1 ||
              callGraphReaches(callGraph, calleeName, caller.name()) ||
              !isGeneralInlineCandidate(found->second)) {
            continue;
          }
          const std::size_t cost = instructionCount(found->second);
          const std::size_t costLimit =
              calleeHasLoop ? kSmallLoopCalleeLimit : kSmallCalleeLimit;
          if (cost == 0 || cost > costLimit ||
              callerGrowth + cost > kCallerGrowthLimit ||
              totalGrowth + cost > kModuleGrowthLimit) {
            continue;
          }
          if (inlineGeneralCallSite(caller, found->second, blockIndex,
                                    instructionIndex)) {
            callerGrowth += cost;
            totalGrowth += cost;
            inlinedCallees.insert(calleeName);
            return true;
          }
        }
      }
    }
  }
  return false;
}

void inlineGeneralCFGCalls(Module& module) {
  constexpr std::size_t kMaximumSites = 16;
  std::size_t totalGrowth = 0;
  std::unordered_set<std::string> inlinedCallees;
  for (std::size_t site = 0; site < kMaximumSites; ++site) {
    if (!inlineOneGeneralCall(module, totalGrowth, inlinedCallees)) {
      break;
    }
  }

  if (inlinedCallees.empty()) {
    return;
  }
  const auto remainingCalls = buildCallGraph(module);
  module.functions().erase(
      std::remove_if(module.functions().begin(), module.functions().end(),
                     [&](const Function& function) {
                       return inlinedCallees.find(function.name()) !=
                                  inlinedCallees.end() &&
                              staticCallCount(remainingCalls,
                                              function.name()) == 0;
                     }),
      module.functions().end());
}

}  // namespace

void FunctionInliningPass::run(compiler::ir::core::Module& module) {
  constexpr std::size_t kMaxInliningRounds = 4;
  for (std::size_t round = 0; round < kMaxInliningRounds; ++round) {
    const auto candidates = collectInlineCandidates(module);
    if (candidates.empty()) {
      break;
    }

    bool changed = false;
    for (auto& function : module.functions()) {
      changed |= inlineFunctionCalls(function, candidates);
    }
    if (!changed) {
      break;
    }
  }
  inlineGeneralCFGCalls(module);
}

}  // namespace compiler::midend::pass
