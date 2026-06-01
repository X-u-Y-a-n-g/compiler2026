#include "../../../include/compiler/midend/pass/FunctionInliningPass.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/core/BasicBlock.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

struct InlineCandidate {
  const Function* function = nullptr;
  std::vector<std::string> parameters;
  std::size_t instructionCount = 0;
};

struct ReturnIncoming {
  std::string value;
  std::string block;
};

bool isValueName(const std::string& value) {
  return !value.empty() && value.front() == '%';
}

bool isInlineableInstruction(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  if (opcode == "br" || opcode == "ret") {
    return true;
  }
  if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
    return false;
  }
  return opcode != "alloca" && opcode != "load" && opcode != "store" && opcode != "phi" &&
         opcode != "call";
}

std::unordered_map<std::string, std::size_t> blockIndexByName(const Function& function) {
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    index.emplace(function.blocks()[i].name(), i);
  }
  return index;
}

std::vector<std::vector<std::size_t>> successors(const Function& function) {
  const auto index = blockIndexByName(function);
  std::vector<std::vector<std::size_t>> succs(function.blocks().size());
  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    const auto* terminator = function.blocks()[i].terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto& operands = terminator->operands();
    if (operands.size() == 1) {
      const auto target = index.find(operands[0]);
      if (target != index.end()) {
        succs[i].push_back(target->second);
      }
    } else if (operands.size() == 3) {
      const auto trueTarget = index.find(operands[1]);
      const auto falseTarget = index.find(operands[2]);
      if (trueTarget != index.end()) {
        succs[i].push_back(trueTarget->second);
      }
      if (falseTarget != index.end() && falseTarget != trueTarget) {
        succs[i].push_back(falseTarget->second);
      }
    }
  }
  return succs;
}

bool hasCycle(const Function& function) {
  const auto succs = successors(function);
  std::vector<int> state(function.blocks().size(), 0);

  const auto dfs = [&](const auto& self, std::size_t block) -> bool {
    state[block] = 1;
    for (const auto succ : succs[block]) {
      if (state[succ] == 1) {
        return true;
      }
      if (state[succ] == 0 && self(self, succ)) {
        return true;
      }
    }
    state[block] = 2;
    return false;
  };

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    if (state[i] == 0 && dfs(dfs, i)) {
      return true;
    }
  }
  return false;
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
  constexpr std::size_t kMaxInlineInstructions = 48;
  std::unordered_map<std::string, InlineCandidate> candidates;
  const auto recursive = collectRecursiveFunctions(module);

  for (const auto& function : module.functions()) {
    if (function.name() == "main" || recursive.find(function.name()) != recursive.end() ||
        function.blocks().empty() || hasCycle(function)) {
      continue;
    }

    InlineCandidate candidate;
    candidate.function = &function;
    for (const auto& parameter : function.parameters()) {
      candidate.parameters.push_back(parameter.name);
    }

    bool supported = true;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!isInlineableInstruction(instruction)) {
          supported = false;
          break;
        }
        ++candidate.instructionCount;
      }
      if (!supported) {
        break;
      }
    }

    if (supported && candidate.instructionCount <= kMaxInlineInstructions) {
      candidates.emplace(function.name(), candidate);
    }
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

std::unordered_set<std::string> collectUsedBlockNames(const Function& function) {
  std::unordered_set<std::string> used;
  for (const auto& block : function.blocks()) {
    used.insert(block.name());
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

std::string freshBlockName(std::unordered_set<std::string>& used, const std::string& original) {
  const std::string stem = "inl." + original;
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

std::vector<std::string> rewriteOperands(
    const Instruction& instruction,
    const std::unordered_map<std::string, std::string>& replacements,
    const std::unordered_map<std::string, std::string>& blockLabels) {
  std::vector<std::string> rewritten;
  rewritten.reserve(instruction.operands().size());

  const auto& operands = instruction.operands();
  if (instruction.opcode() == "br") {
    if (operands.size() == 1) {
      rewritten.push_back(blockLabels.at(operands[0]));
      return rewritten;
    }
    if (operands.size() == 3) {
      rewritten.push_back(rewriteValue(operands[0], replacements));
      rewritten.push_back(blockLabels.at(operands[1]));
      rewritten.push_back(blockLabels.at(operands[2]));
      return rewritten;
    }
  }

  for (const auto& operand : operands) {
    rewritten.push_back(rewriteValue(operand, replacements));
  }
  return rewritten;
}

void rewritePhiPredecessor(Function& function, const std::string& oldPred,
                           const std::string& newPred) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        break;
      }

      auto operands = instruction.operands();
      for (std::size_t i = 2; i < operands.size(); i += 2) {
        if (operands[i] == oldPred) {
          operands[i] = newPred;
        }
      }
      instruction = Instruction{"phi", std::move(operands), instruction.result()};
    }
  }
}

bool inlineCallAt(Function& caller, std::size_t blockIndex, std::size_t instructionIndex,
                  const InlineCandidate& candidate,
                  const std::vector<std::string>& callOperands,
                  const std::string& callResult) {
  if (candidate.function == nullptr ||
      callOperands.size() - 1 != candidate.parameters.size()) {
    return false;
  }

  auto usedValues = collectUsedValueNames(caller);
  auto usedBlocks = collectUsedBlockNames(caller);

  const auto& callee = *candidate.function;
  std::unordered_map<std::string, std::string> replacements;
  for (std::size_t i = 0; i < candidate.parameters.size(); ++i) {
    replacements.emplace(candidate.parameters[i], callOperands[i + 1]);
  }

  std::unordered_map<std::string, std::string> blockLabels;
  for (const auto& block : callee.blocks()) {
    blockLabels.emplace(block.name(), freshBlockName(usedBlocks, block.name()));
  }

  const std::string continuationName = freshBlockName(usedBlocks, "cont");
  std::vector<ReturnIncoming> returns;
  std::vector<BasicBlock> clonedBlocks;
  clonedBlocks.reserve(callee.blocks().size());

  for (const auto& calleeBlock : callee.blocks()) {
    BasicBlock cloned{blockLabels.at(calleeBlock.name())};
    for (const auto& instruction : calleeBlock.instructions()) {
      if (instruction.opcode() == "ret") {
        if (!instruction.operands().empty()) {
          returns.push_back(
              ReturnIncoming{rewriteValue(instruction.operands().front(), replacements),
                             cloned.name()});
        }
        cloned.addInstruction(Instruction{"br", {continuationName}});
        break;
      }

      std::string result;
      if (instruction.hasResult()) {
        result = freshValueName(usedValues, instruction.result());
        replacements.emplace(instruction.result(), result);
      }
      cloned.addInstruction(Instruction{instruction.opcode(),
                                        rewriteOperands(instruction, replacements, blockLabels),
                                        std::move(result)});
      if (instruction.isTerminator()) {
        break;
      }
    }
    clonedBlocks.push_back(std::move(cloned));
  }

  auto& callerBlock = caller.blocks()[blockIndex];
  const auto originalBlockName = callerBlock.name();
  const auto oldInstructions = callerBlock.instructions();
  std::vector<Instruction> prefix;
  prefix.reserve(instructionIndex + 1);
  for (std::size_t i = 0; i < instructionIndex; ++i) {
    prefix.push_back(oldInstructions[i]);
  }
  prefix.emplace_back("br", std::vector<std::string>{blockLabels.at(callee.blocks().front().name())});
  callerBlock.instructions() = std::move(prefix);

  BasicBlock continuation{continuationName};
  if (!callResult.empty() && callee.returnType() != "void") {
    std::vector<std::string> phiOperands;
    phiOperands.push_back(callee.returnType());
    for (const auto& incoming : returns) {
      phiOperands.push_back(incoming.value);
      phiOperands.push_back(incoming.block);
    }
    continuation.addInstruction(Instruction{"phi", std::move(phiOperands), callResult});
  }
  for (std::size_t i = instructionIndex + 1; i < oldInstructions.size(); ++i) {
    continuation.addInstruction(oldInstructions[i]);
  }

  rewritePhiPredecessor(caller, originalBlockName, continuationName);

  for (auto& block : clonedBlocks) {
    caller.blocks().push_back(std::move(block));
  }
  caller.blocks().push_back(std::move(continuation));
  return true;
}

bool inlineFunctionCalls(
    Function& function, const std::unordered_map<std::string, InlineCandidate>& candidates) {
  bool changed = false;

  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size(); ++blockIndex) {
    const auto instructions = function.blocks()[blockIndex].instructions();
    for (std::size_t instructionIndex = 0; instructionIndex < instructions.size();
         ++instructionIndex) {
      const auto& instruction = instructions[instructionIndex];
      if (instruction.opcode() != "call" || instruction.operands().empty()) {
        continue;
      }

      const auto& operands = instruction.operands();
      const std::string callee =
          !operands.front().empty() && operands.front().front() == '@'
              ? operands.front().substr(1)
              : operands.front();
      const auto candidate = candidates.find(callee);
      if (candidate == candidates.end() || candidate->second.function == &function) {
        continue;
      }

      if (inlineCallAt(function, blockIndex, instructionIndex, candidate->second, operands,
                       instruction.result())) {
        changed = true;
        break;
      }
    }
  }

  return changed;
}

}  // namespace

void FunctionInliningPass::run(compiler::ir::core::Module& module) {
  const auto candidates = collectInlineCandidates(module);
  if (candidates.empty()) {
    return;
  }

  for (auto& function : module.functions()) {
    while (inlineFunctionCalls(function, candidates)) {
    }
  }
}

}  // namespace compiler::midend::pass
