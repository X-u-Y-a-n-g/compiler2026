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

bool sameValue(const std::string& lhs, const std::string& rhs) {
  return lhs == rhs && !lhs.empty();
}

std::optional<std::string> simplifyIntegerBinary(const std::string& opcode,
                                                 const std::vector<std::string>& operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto& lhs = operands[0];
  const auto& rhs = operands[1];
  if (opcode == "add") {
    if (detail::isZero(lhs)) {
      return rhs;
    }
    if (detail::isZero(rhs)) {
      return lhs;
    }
  }
  if (opcode == "sub") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (sameValue(lhs, rhs)) {
      return "0";
    }
  }
  if (opcode == "mul") {
    if (detail::isZero(lhs) || detail::isZero(rhs)) {
      return "0";
    }
    if (detail::isOne(lhs)) {
      return rhs;
    }
    if (detail::isOne(rhs)) {
      return lhs;
    }
  }
  if (opcode == "div" || opcode == "sdiv") {
    if (detail::isOne(rhs)) {
      return lhs;
    }
  }
  if (opcode == "mod" || opcode == "srem") {
    if (detail::isOne(rhs)) {
      return "0";
    }
  }
  if (opcode == "and") {
    if (detail::isZero(lhs) || detail::isZero(rhs)) {
      return "0";
    }
    if (sameValue(lhs, rhs)) {
      return lhs;
    }
  }
  if (opcode == "or") {
    if (detail::isZero(lhs)) {
      return rhs;
    }
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (sameValue(lhs, rhs)) {
      return lhs;
    }
  }

  return std::nullopt;
}

std::optional<std::string> simplifyFloatBinary(const std::string& opcode,
                                               const std::vector<std::string>& operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto& lhs = operands[0];
  const auto& rhs = operands[1];
  if (opcode == "fadd") {
    if (detail::isZero(lhs)) {
      return rhs;
    }
    if (detail::isZero(rhs)) {
      return lhs;
    }
  }
  if (opcode == "fsub") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
  }
  if (opcode == "fmul") {
    if (detail::isOne(lhs)) {
      return rhs;
    }
    if (detail::isOne(rhs)) {
      return lhs;
    }
  }
  if (opcode == "fdiv") {
    if (detail::isOne(rhs)) {
      return lhs;
    }
  }

  return std::nullopt;
}

std::optional<std::string> simplifyIntegerCompare(const std::string& opcode,
                                                  const std::vector<std::string>& operands) {
  if (operands.size() != 2 || opcode.rfind("icmp.", 0) != 0 ||
      !sameValue(operands[0], operands[1])) {
    return std::nullopt;
  }

  const std::string predicate = opcode.substr(5);
  if (predicate == "eq" || predicate == "sle" || predicate == "sge") {
    return "1";
  }
  if (predicate == "ne" || predicate == "slt" || predicate == "sgt") {
    return "0";
  }
  return std::nullopt;
}

std::optional<std::string> simplifyInstruction(const Instruction& instruction) {
  if (!instruction.hasResult()) {
    return std::nullopt;
  }

  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
      opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
      opcode == "or") {
    return simplifyIntegerBinary(opcode, operands);
  }
  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    return simplifyFloatBinary(opcode, operands);
  }
  return simplifyIntegerCompare(opcode, operands);
}

void simplifyFunction(Function& function) {
  std::unordered_map<std::string, std::string> aliases;

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      Instruction current{instruction.opcode(),
                          detail::rewriteOperands(instruction.operands(), aliases),
                          instruction.result()};

      if (current.hasResult()) {
        if (auto replacement = simplifyInstruction(current); replacement.has_value()) {
          aliases[current.result()] = detail::resolveAlias(*replacement, aliases);
          continue;
        }
        aliases.erase(current.result());
      }

      rewritten.push_back(std::move(current));
    }

    block.instructions() = std::move(rewritten);
  }
}

}  // namespace

bool AlgebraicSimplificationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    simplifyFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
