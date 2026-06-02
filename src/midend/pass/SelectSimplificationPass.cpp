#include "../../../include/compiler/midend/pass/SelectSimplificationPass.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

void rewriteFunctionOperands(
    Function& function, const std::unordered_map<std::string, std::string>& aliases) {
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

bool isTrueConstant(const std::string& operand) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value != 0;
}

bool isFalseConstant(const std::string& operand) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value == 0;
}

bool runSelectSimplify(Function& function) {
  bool changed = false;
  std::unordered_map<std::string, std::string> aliases;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      if (instruction.opcode() == "select" && instruction.hasResult() && operands.size() == 4) {
        const auto& condition = operands[1];
        const auto& trueValue = operands[2];
        const auto& falseValue = operands[3];
        if (trueValue == falseValue) {
          aliases[instruction.result()] = trueValue;
          changed = true;
          continue;
        }
        if (isTrueConstant(condition)) {
          aliases[instruction.result()] = trueValue;
          changed = true;
          continue;
        }
        if (isFalseConstant(condition)) {
          aliases[instruction.result()] = falseValue;
          changed = true;
          continue;
        }
      }
      if (instruction.hasResult()) {
        aliases.erase(instruction.result());
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
  rewriteFunctionOperands(function, aliases);
  return changed;
}

}  // namespace

bool SelectSimplificationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runSelectSimplify(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
