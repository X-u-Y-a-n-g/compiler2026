#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/ir/core/Instruction.h"
#include "../../../include/compiler/ir/core/Module.h"

namespace compiler::midend::analysis {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

struct DirectFunctionEffects {
  FunctionEffects effects;
  std::vector<std::string> callees;
};

std::string calledFunctionName(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& callee = instruction.operands().front();
  if (callee.size() < 2 || callee.front() != '@') {
    return {};
  }
  return callee.substr(1);
}

bool isLocalPointerDerivation(
    const Instruction& instruction,
    const std::unordered_set<std::string>& localPointers) {
  if (!instruction.hasResult()) {
    return false;
  }
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  if (opcode == "getelementptr" || opcode == "getelementptr.linear" ||
      opcode == "gep" || opcode == "gep.linear" || opcode == "cast") {
    return !operands.empty() &&
           localPointers.find(operands.front()) != localPointers.end();
  }
  if (opcode == "select" && operands.size() == 3) {
    return localPointers.find(operands[1]) != localPointers.end() &&
           localPointers.find(operands[2]) != localPointers.end();
  }
  if (opcode != "phi") {
    return false;
  }

  bool sawLocalPointer = false;
  for (const auto& operand : operands) {
    if (operand.empty() || operand.front() != '%') {
      continue;
    }
    if (localPointers.find(operand) == localPointers.end()) {
      return false;
    }
    sawLocalPointer = true;
  }
  return sawLocalPointer;
}

std::unordered_set<std::string> findLocalPointers(const Function& function) {
  std::unordered_set<std::string> localPointers;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        localPointers.insert(instruction.result());
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (isLocalPointerDerivation(instruction, localPointers)) {
          changed |= localPointers.insert(instruction.result()).second;
        }
      }
    }
  }
  return localPointers;
}

bool containsLocalPointer(
    const std::vector<std::string>& operands, std::size_t begin,
    const std::unordered_set<std::string>& localPointers) {
  for (std::size_t index = begin; index < operands.size(); ++index) {
    if (localPointers.find(operands[index]) != localPointers.end()) {
      return true;
    }
  }
  return false;
}

DirectFunctionEffects analyzeDirectEffects(
    const Function& function,
    const std::unordered_set<std::string>& definedFunctions) {
  DirectFunctionEffects result;
  if (function.blocks().empty()) {
    result.effects.readsExternalMemory = true;
    result.effects.writesExternalMemory = true;
    result.effects.hasOtherSideEffects = true;
    result.effects.readsUnknownMemory = true;
    result.effects.writesUnknownMemory = true;
    return result;
  }
  const auto localPointers = findLocalPointers(function);

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      const auto& opcode = instruction.opcode();
      const auto& operands = instruction.operands();
      if (opcode == "load") {
        const bool local = !operands.empty() &&
                           localPointers.find(operands.back()) !=
                               localPointers.end();
        result.effects.readsExternalMemory |= !local;
        if (!local && !operands.empty() && !operands.back().empty() &&
            operands.back().front() == '@') {
          result.effects.readSymbols.insert(operands.back());
        } else if (!local) {
          result.effects.readsUnknownMemory = true;
        }
        continue;
      }
      if (opcode == "store") {
        const bool localDestination =
            !operands.empty() &&
            localPointers.find(operands.back()) != localPointers.end();
        result.effects.writesExternalMemory |= !localDestination;
        if (!localDestination && !operands.empty() &&
            !operands.back().empty() && operands.back().front() == '@') {
          result.effects.writtenSymbols.insert(operands.back());
        } else if (!localDestination) {
          result.effects.writesUnknownMemory = true;
        }
        const bool storesLocalPointer =
            operands.size() >= 2 &&
            localPointers.find(operands[operands.size() - 2]) !=
                localPointers.end();
        if (storesLocalPointer) {
          result.effects.hasOtherSideEffects = true;
        }
        continue;
      }
      if (opcode == "ret") {
        if (containsLocalPointer(operands, 0, localPointers)) {
          result.effects.hasOtherSideEffects = true;
        }
        continue;
      }
      if (opcode != "call") {
        continue;
      }

      if (containsLocalPointer(operands, 1, localPointers)) {
        result.effects.hasOtherSideEffects = true;
      }
      const auto callee = calledFunctionName(instruction);
      if (!callee.empty() && definedFunctions.find(callee) != definedFunctions.end()) {
        result.callees.push_back(callee);
        continue;
      }
      result.effects.readsExternalMemory = true;
      result.effects.writesExternalMemory = true;
      result.effects.hasOtherSideEffects = true;
      result.effects.readsUnknownMemory = true;
      result.effects.writesUnknownMemory = true;
    }
  }
  return result;
}

}  // namespace

bool FunctionEffects::merge(const FunctionEffects& other) {
  const FunctionEffects before = *this;
  readsExternalMemory |= other.readsExternalMemory;
  writesExternalMemory |= other.writesExternalMemory;
  hasOtherSideEffects |= other.hasOtherSideEffects;
  readsUnknownMemory |= other.readsUnknownMemory;
  writesUnknownMemory |= other.writesUnknownMemory;
  readSymbols.insert(other.readSymbols.begin(), other.readSymbols.end());
  writtenSymbols.insert(other.writtenSymbols.begin(), other.writtenSymbols.end());
  return readsExternalMemory != before.readsExternalMemory ||
         writesExternalMemory != before.writesExternalMemory ||
         hasOtherSideEffects != before.hasOtherSideEffects ||
         readsUnknownMemory != before.readsUnknownMemory ||
         writesUnknownMemory != before.writesUnknownMemory ||
         readSymbols != before.readSymbols ||
         writtenSymbols != before.writtenSymbols;
}

FunctionEffectAnalysis::FunctionEffectAnalysis(const Module& module) {
  std::unordered_set<std::string> definedFunctions;
  for (const auto& function : module.functions()) {
    definedFunctions.insert(function.name());
  }

  std::unordered_map<std::string, DirectFunctionEffects> directEffects;
  for (const auto& function : module.functions()) {
    auto direct = analyzeDirectEffects(function, definedFunctions);
    effects_.emplace(function.name(), direct.effects);
    directEffects.emplace(function.name(), std::move(direct));
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [functionName, direct] : directEffects) {
      auto& functionEffects = effects_.at(functionName);
      const auto before = functionEffects;
      FunctionEffects recomputed = direct.effects;
      for (const auto& callee : direct.callees) {
        recomputed.merge(effects_.at(callee));
      }
      if (recomputed.readsExternalMemory != before.readsExternalMemory ||
          recomputed.writesExternalMemory != before.writesExternalMemory ||
          recomputed.hasOtherSideEffects != before.hasOtherSideEffects ||
          recomputed.readsUnknownMemory != before.readsUnknownMemory ||
          recomputed.writesUnknownMemory != before.writesUnknownMemory ||
          recomputed.readSymbols != before.readSymbols ||
          recomputed.writtenSymbols != before.writtenSymbols) {
        functionEffects = recomputed;
        changed = true;
      }
    }
  }
}

const FunctionEffects& FunctionEffectAnalysis::effectFor(
    std::string_view functionName) const {
  const auto found = effects_.find(std::string(functionName));
  if (found != effects_.end()) {
    return found->second;
  }
  static const FunctionEffects unknown = [] {
    FunctionEffects effects;
    effects.readsExternalMemory = true;
    effects.writesExternalMemory = true;
    effects.hasOtherSideEffects = true;
    effects.readsUnknownMemory = true;
    effects.writesUnknownMemory = true;
    return effects;
  }();
  return unknown;
}

bool FunctionEffectAnalysis::isReadNone(std::string_view functionName) const {
  return effectFor(functionName).isReadNone();
}

}  // namespace compiler::midend::analysis
