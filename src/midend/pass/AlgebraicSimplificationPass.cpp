#include "../../../include/compiler/midend/pass/AlgebraicSimplificationPass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

std::optional<std::string> simplifyInstruction(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  if (!instruction.hasResult() || operands.size() != 2) {
    return std::nullopt;
  }

  if (opcode == "add") {
    if (detail::isZero(operands[0])) {
      return operands[1];
    }
    if (detail::isZero(operands[1])) {
      return operands[0];
    }
  }
  if (opcode == "sub" && detail::isZero(operands[1])) {
    return operands[0];
  }
  if (opcode == "mul") {
    if (detail::isZero(operands[0]) || detail::isZero(operands[1])) {
      return "0";
    }
    if (detail::isOne(operands[0])) {
      return operands[1];
    }
    if (detail::isOne(operands[1])) {
      return operands[0];
    }
  }
  if ((opcode == "div" || opcode == "sdiv") && detail::isOne(operands[1])) {
    return operands[0];
  }
  if ((opcode == "mod" || opcode == "srem") && detail::isOne(operands[1])) {
    return "0";
  }
  if (opcode == "and" && (detail::isZero(operands[0]) || detail::isZero(operands[1]))) {
    return "0";
  }
  if (opcode == "or") {
    if (detail::isZero(operands[0])) {
      return operands[1];
    }
    if (detail::isZero(operands[1])) {
      return operands[0];
    }
  }
  if (opcode == "fadd") {
    if (detail::isZero(operands[0])) {
      return operands[1];
    }
    if (detail::isZero(operands[1])) {
      return operands[0];
    }
  }
  if (opcode == "fsub" && detail::isZero(operands[1])) {
    return operands[0];
  }
  if (opcode == "fmul") {
    if (detail::isOne(operands[0])) {
      return operands[1];
    }
    if (detail::isOne(operands[1])) {
      return operands[0];
    }
  }
  if (opcode == "fdiv" && detail::isOne(operands[1])) {
    return operands[0];
  }

  return std::nullopt;
}

void simplifyFunction(Function& function) {
  std::unordered_map<std::string, std::string> aliases;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewrittenInstructions;
    rewrittenInstructions.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      const auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      Instruction rewritten{instruction.opcode(), operands, instruction.result()};

      if (rewritten.hasResult()) {
        if (auto replacement = simplifyInstruction(rewritten); replacement.has_value()) {
          aliases[rewritten.result()] = detail::resolveAlias(*replacement, aliases);
          continue;
        }
        aliases.erase(rewritten.result());
      }

      rewrittenInstructions.push_back(std::move(rewritten));
    }

    block.instructions() = std::move(rewrittenInstructions);
  }
}

}  // namespace

void AlgebraicSimplificationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    simplifyFunction(function);
  }
}

}  // namespace compiler::midend::pass
