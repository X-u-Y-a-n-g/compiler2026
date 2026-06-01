#include "../../../include/compiler/midend/pass/FunctionInliningPass.h"

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
  constexpr std::size_t kMaxInlineInstructions = 8;
  std::unordered_map<std::string, InlineCandidate> candidates;
  const auto recursive = collectRecursiveFunctions(module);

  for (const auto& function : module.functions()) {
    if (function.name() == "main" || function.returnType() == "void" ||
        recursive.find(function.name()) != recursive.end() || function.blocks().size() != 1) {
      continue;
    }

    const auto& instructions = function.blocks().front().instructions();
    if (instructions.empty()) {
      continue;
    }

    const auto& terminator = instructions.back();
    if (terminator.opcode() != "ret" || terminator.operands().size() != 1) {
      continue;
    }
    if (instructions.size() - 1 > kMaxInlineInstructions) {
      continue;
    }

    InlineCandidate candidate;
    candidate.name = function.name();
    candidate.returnValue = terminator.operands().front();
    for (const auto& parameter : function.parameters()) {
      candidate.parameters.push_back(parameter.name);
    }

    bool supported = true;
    for (std::size_t i = 0; i + 1 < instructions.size(); ++i) {
      if (!isPureInlineInstruction(instructions[i])) {
        supported = false;
        break;
      }
      candidate.instructions.push_back(instructions[i]);
    }
    if (!supported) {
      continue;
    }

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

      if (instruction.hasResult()) {
        aliases[instruction.result()] = rewriteValue(candidate.returnValue, replacements);
      }
      changed = true;
    }

    block.instructions() = std::move(rewritten);
  }

  rewriteFunctionOperands(function, aliases);
  return changed;
}

}  // namespace

void FunctionInliningPass::run(compiler::ir::core::Module& module) {
  const auto candidates = collectInlineCandidates(module);
  if (candidates.empty()) {
    return;
  }

  for (auto& function : module.functions()) {
    inlineFunctionCalls(function, candidates);
  }
}

}  // namespace compiler::midend::pass
