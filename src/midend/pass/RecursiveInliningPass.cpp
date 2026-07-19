#include "../../../include/compiler/midend/pass/RecursiveInliningPass.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

constexpr std::size_t kMaximumInlineDepth = 2;
constexpr std::size_t kMaximumClonedInstructions = 224;

bool isDirectSelfCall(const Instruction &instruction,
                      const Function &function) {
  return instruction.opcode() == "call" && instruction.hasResult() &&
         !instruction.operands().empty() &&
         instruction.operands().front() == "@" + function.name() &&
         instruction.operands().size() == function.parameters().size() + 1;
}

std::size_t instructionCount(const Function &function) {
  std::size_t count = 0;
  for (const auto &block : function.blocks()) {
    count += block.instructions().size();
  }
  return count;
}

std::size_t recursiveCallCount(const Function &function) {
  std::size_t count = 0;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      count += isDirectSelfCall(instruction, function) ? 1 : 0;
    }
  }
  return count;
}

bool hasSupportedReturns(const Function &function) {
  if (function.returnType() == "void") {
    return false;
  }
  bool sawReturn = false;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "ret") {
        continue;
      }
      if (instruction.operands().size() != 1) {
        return false;
      }
      sawReturn = true;
    }
  }
  return sawReturn;
}

std::size_t profitableDepth(const Function &function) {
  const std::size_t bodySize = instructionCount(function);
  const std::size_t calls = recursiveCallCount(function);
  if (bodySize == 0 || calls == 0 || !hasSupportedReturns(function)) {
    return 0;
  }

  std::size_t depth = 0;
  std::size_t callsAtDepth = 1;
  std::size_t totalClones = 0;
  for (std::size_t candidateDepth = 1; candidateDepth <= kMaximumInlineDepth;
       ++candidateDepth) {
    if (callsAtDepth > kMaximumClonedInstructions / calls) {
      break;
    }
    callsAtDepth *= calls;
    if (totalClones > kMaximumClonedInstructions - callsAtDepth) {
      break;
    }
    totalClones += callsAtDepth;
    if (bodySize > kMaximumClonedInstructions / totalClones) {
      break;
    }
    depth = candidateDepth;
  }
  return depth;
}

std::unordered_set<std::string> collectValueNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshValueName(std::unordered_set<std::string> &names,
                           const std::string &original) {
  const std::string stem =
      "%rinl." + (!original.empty() && original.front() == '%'
                      ? original.substr(1)
                      : std::string("value"));
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string freshBlockName(std::unordered_set<std::string> &names,
                           const std::string &original) {
  const std::string stem = "rinl." + original;
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string rewriteValue(
    const std::string &value,
    const std::unordered_map<std::string, std::string> &valueReplacements) {
  const auto found = valueReplacements.find(value);
  return found == valueReplacements.end() ? value : found->second;
}

std::string rewriteBlock(
    const std::string &block,
    const std::unordered_map<std::string, std::string> &blockReplacements) {
  const auto found = blockReplacements.find(block);
  return found == blockReplacements.end() ? block : found->second;
}

std::vector<std::string> rewriteInstructionOperands(
    const Instruction &instruction,
    const std::unordered_map<std::string, std::string> &valueReplacements,
    const std::unordered_map<std::string, std::string> &blockReplacements) {
  const auto &operands = instruction.operands();
  if (instruction.opcode() == "br") {
    if (operands.size() == 1) {
      return {rewriteBlock(operands.front(), blockReplacements)};
    }
    if (operands.size() == 3) {
      return {rewriteValue(operands[0], valueReplacements),
              rewriteBlock(operands[1], blockReplacements),
              rewriteBlock(operands[2], blockReplacements)};
    }
  }

  if (instruction.opcode() == "phi" && !operands.empty()) {
    std::vector<std::string> rewritten;
    rewritten.reserve(operands.size());
    rewritten.push_back(operands.front());
    for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
      rewritten.push_back(rewriteValue(operands[index], valueReplacements));
      rewritten.push_back(rewriteBlock(operands[index + 1], blockReplacements));
    }
    return rewritten;
  }

  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());
  for (const auto &operand : operands) {
    rewritten.push_back(rewriteValue(operand, valueReplacements));
  }
  return rewritten;
}

std::vector<std::string>
branchSuccessors(const std::vector<Instruction> &suffix) {
  if (suffix.empty()) {
    return {};
  }
  const auto &terminator = suffix.back();
  if (terminator.opcode() != "br") {
    return {};
  }
  if (terminator.operands().size() == 1) {
    return {terminator.operands().front()};
  }
  if (terminator.operands().size() == 3) {
    return {terminator.operands()[1], terminator.operands()[2]};
  }
  return {};
}

void replacePhiPredecessor(Function &function,
                           const std::vector<std::string> &successors,
                           const std::string &oldPredecessor,
                           const std::string &newPredecessor) {
  if (successors.empty()) {
    return;
  }
  const std::unordered_set<std::string> successorSet(successors.begin(),
                                                     successors.end());
  for (auto &block : function.blocks()) {
    if (successorSet.find(block.name()) == successorSet.end()) {
      continue;
    }
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldPredecessor) {
          operands[index] = newPredecessor;
        }
      }
      rewritten.emplace_back("phi", std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

bool inlineCallSite(Function &function, const Function &snapshot,
                    std::size_t blockIndex, std::size_t callIndex,
                    std::size_t callerDepth,
                    std::unordered_map<std::string, std::size_t> &blockDepths,
                    std::unordered_set<std::string> &valueNames,
                    std::unordered_set<std::string> &blockNames) {
  const auto call = function.blocks()[blockIndex].instructions()[callIndex];
  if (!isDirectSelfCall(call, snapshot)) {
    return false;
  }

  const std::string callerBlockName = function.blocks()[blockIndex].name();
  const std::string continuationName =
      freshBlockName(blockNames, "continuation");

  std::unordered_map<std::string, std::string> blockReplacements;
  for (const auto &block : snapshot.blocks()) {
    blockReplacements.emplace(block.name(),
                              freshBlockName(blockNames, block.name()));
  }

  std::unordered_map<std::string, std::string> valueReplacements;
  for (std::size_t index = 0; index < snapshot.parameters().size(); ++index) {
    valueReplacements.emplace(snapshot.parameters()[index].name,
                              call.operands()[index + 1]);
  }
  for (const auto &block : snapshot.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        valueReplacements.emplace(
            instruction.result(),
            freshValueName(valueNames, instruction.result()));
      }
    }
  }

  std::vector<BasicBlock> clonedBlocks;
  clonedBlocks.reserve(snapshot.blocks().size());
  std::vector<std::pair<std::string, std::string>> returns;
  for (const auto &sourceBlock : snapshot.blocks()) {
    BasicBlock cloned{blockReplacements.at(sourceBlock.name())};
    for (const auto &instruction : sourceBlock.instructions()) {
      if (instruction.opcode() == "ret") {
        returns.emplace_back(
            rewriteValue(instruction.operands().front(), valueReplacements),
            cloned.name());
        cloned.addInstruction(Instruction{"br", {continuationName}});
        continue;
      }
      const std::string result =
          instruction.hasResult() ? valueReplacements.at(instruction.result())
                                  : "";
      cloned.addInstruction(
          Instruction{instruction.opcode(),
                      rewriteInstructionOperands(instruction, valueReplacements,
                                                 blockReplacements),
                      result});
    }
    clonedBlocks.push_back(std::move(cloned));
  }
  if (returns.empty()) {
    return false;
  }

  auto &callerInstructions = function.blocks()[blockIndex].instructions();
  std::vector<Instruction> suffix(
      callerInstructions.begin() + static_cast<std::ptrdiff_t>(callIndex + 1),
      callerInstructions.end());
  const auto successors = branchSuccessors(suffix);
  callerInstructions.erase(callerInstructions.begin() +
                               static_cast<std::ptrdiff_t>(callIndex),
                           callerInstructions.end());
  callerInstructions.emplace_back(
      "br", std::vector<std::string>{
                blockReplacements.at(snapshot.blocks().front().name())});

  replacePhiPredecessor(function, successors, callerBlockName,
                        continuationName);

  auto insertionPoint =
      function.blocks().begin() + static_cast<std::ptrdiff_t>(blockIndex + 1);
  for (auto &block : clonedBlocks) {
    blockDepths.emplace(block.name(), callerDepth + 1);
    insertionPoint = function.blocks().insert(insertionPoint, std::move(block));
    ++insertionPoint;
  }

  BasicBlock continuation{continuationName};
  std::vector<std::string> phiOperands{snapshot.returnType()};
  for (const auto &[value, predecessor] : returns) {
    phiOperands.push_back(value);
    phiOperands.push_back(predecessor);
  }
  continuation.addInstruction(
      Instruction{"phi", std::move(phiOperands), call.result()});
  for (auto &instruction : suffix) {
    continuation.addInstruction(std::move(instruction));
  }
  blockDepths.emplace(continuationName, callerDepth);
  function.blocks().insert(insertionPoint, std::move(continuation));
  return true;
}

bool inlineRecursiveCalls(Function &function) {
  const std::size_t maximumDepth = profitableDepth(function);
  if (maximumDepth == 0) {
    return false;
  }

  const Function snapshot = function;
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  std::unordered_map<std::string, std::size_t> blockDepths;
  for (const auto &block : function.blocks()) {
    blockDepths.emplace(block.name(), 0);
  }

  bool changed = false;
  for (;;) {
    bool found = false;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      const auto depthIt =
          blockDepths.find(function.blocks()[blockIndex].name());
      if (depthIt == blockDepths.end() || depthIt->second >= maximumDepth) {
        continue;
      }
      const std::size_t callerDepth = depthIt->second;
      for (std::size_t instructionIndex = 0;
           instructionIndex <
           function.blocks()[blockIndex].instructions().size();
           ++instructionIndex) {
        if (!isDirectSelfCall(
                function.blocks()[blockIndex].instructions()[instructionIndex],
                snapshot)) {
          continue;
        }
        if (inlineCallSite(function, snapshot, blockIndex, instructionIndex,
                           callerDepth, blockDepths, valueNames, blockNames)) {
          changed = true;
          found = true;
        }
        break;
      }
      if (found) {
        break;
      }
    }
    if (!found) {
      break;
    }
  }
  return changed;
}

} // namespace

void RecursiveInliningPass::run(compiler::ir::core::Module &module) {
  for (auto &function : module.functions()) {
    inlineRecursiveCalls(function);
  }
}

} // namespace compiler::midend::pass
