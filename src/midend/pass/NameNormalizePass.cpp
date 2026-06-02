#include "../../../include/compiler/midend/pass/NameNormalizePass.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

bool isLocalValue(const std::string& operand) {
  return !operand.empty() && operand.front() == '%';
}

bool isTypeOperandPosition(const Instruction& instruction, std::size_t index) {
  const auto& opcode = instruction.opcode();
  if (opcode == "alloca") {
    return index == 0;
  }
  if (opcode == "load") {
    return index == 0;
  }
  if (opcode == "store") {
    return index == 0;
  }
  if (opcode == "phi") {
    return index == 0;
  }
  if (opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
      opcode == "sitofp" || opcode == "uitofp" || opcode == "fptosi" ||
      opcode == "cast") {
    return index == 1;
  }
  return false;
}

std::string rewriteOperand(const Instruction& instruction, std::size_t index,
                           const std::unordered_map<std::string, std::string>& valueNames,
                           const std::unordered_map<std::string, std::string>& blockNames) {
  const auto& operand = instruction.operands()[index];
  if (isTypeOperandPosition(instruction, index)) {
    return operand;
  }
  if (const auto found = valueNames.find(operand); found != valueNames.end()) {
    return found->second;
  }
  if (const auto found = blockNames.find(operand); found != blockNames.end()) {
    return found->second;
  }
  return operand;
}

std::vector<std::string> rewriteOperands(
    const Instruction& instruction,
    const std::unordered_map<std::string, std::string>& valueNames,
    const std::unordered_map<std::string, std::string>& blockNames) {
  std::vector<std::string> operands;
  operands.reserve(instruction.operands().size());
  for (std::size_t i = 0; i < instruction.operands().size(); ++i) {
    operands.push_back(rewriteOperand(instruction, i, valueNames, blockNames));
  }
  return operands;
}

bool normalizeFunction(Function& function) {
  std::unordered_map<std::string, std::string> blockNames;
  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    const std::string next = i == 0 ? "entry" : "bb" + std::to_string(i);
    blockNames.emplace(function.blocks()[i].name(), next);
  }

  std::unordered_map<std::string, std::string> valueNames;
  std::size_t valueIndex = 0;
  for (const auto& parameter : function.parameters()) {
    if (isLocalValue(parameter.name)) {
      valueNames.emplace(parameter.name, "%arg" + std::to_string(valueIndex++));
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        valueNames.emplace(instruction.result(), "%v" + std::to_string(valueIndex++));
      }
    }
  }

  bool changed = false;
  for (auto& parameter : function.parameters()) {
    const auto found = valueNames.find(parameter.name);
    if (found != valueNames.end() && parameter.name != found->second) {
      parameter.name = found->second;
      changed = true;
    }
  }

  for (auto& block : function.blocks()) {
    const auto blockName = blockNames.find(block.name());
    const std::string newBlockName =
        blockName == blockNames.end() ? block.name() : blockName->second;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      auto operands = rewriteOperands(instruction, valueNames, blockNames);
      std::string result = instruction.result();
      if (instruction.hasResult()) {
        if (const auto found = valueNames.find(instruction.result()); found != valueNames.end()) {
          result = found->second;
        }
      }
      changed |= operands != instruction.operands() || result != instruction.result();
      rewritten.emplace_back(instruction.opcode(), std::move(operands), std::move(result));
    }
    changed |= block.name() != newBlockName;
    compiler::ir::core::BasicBlock renamed{newBlockName};
    for (auto& instruction : rewritten) {
      renamed.addInstruction(std::move(instruction));
    }
    block = std::move(renamed);
  }
  return changed;
}

}  // namespace

bool NameNormalizePass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    normalizeFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
