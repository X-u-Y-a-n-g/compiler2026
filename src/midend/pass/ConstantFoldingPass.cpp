#include "../../../include/compiler/midend/pass/ConstantFoldingPass.h"

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

std::optional<std::string> foldIntegerBinary(const std::string& opcode,
                                             const std::vector<std::string>& operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  long long lhs = 0;
  long long rhs = 0;
  if (!detail::parseIntegerLiteral(operands[0], lhs) ||
      !detail::parseIntegerLiteral(operands[1], rhs)) {
    return std::nullopt;
  }

  if (opcode == "add") {
    return detail::formatIntegerLiteral(lhs + rhs);
  }
  if (opcode == "sub") {
    return detail::formatIntegerLiteral(lhs - rhs);
  }
  if (opcode == "mul") {
    return detail::formatIntegerLiteral(lhs * rhs);
  }
  if (opcode == "div" || opcode == "sdiv") {
    return rhs == 0 ? std::nullopt : std::optional<std::string>{detail::formatIntegerLiteral(lhs / rhs)};
  }
  if (opcode == "mod" || opcode == "srem") {
    return rhs == 0 ? std::nullopt : std::optional<std::string>{detail::formatIntegerLiteral(lhs % rhs)};
  }
  if (opcode == "and") {
    return detail::formatIntegerLiteral(lhs & rhs);
  }
  if (opcode == "or") {
    return detail::formatIntegerLiteral(lhs | rhs);
  }
  if (opcode == "xor") {
    return detail::formatIntegerLiteral(lhs ^ rhs);
  }
  return std::nullopt;
}

std::optional<std::string> foldFloatBinary(const std::string& opcode,
                                           const std::vector<std::string>& operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  double lhs = 0.0;
  double rhs = 0.0;
  if (!detail::parseFloatLiteral(operands[0], lhs) ||
      !detail::parseFloatLiteral(operands[1], rhs)) {
    return std::nullopt;
  }

  if (opcode == "fadd") {
    return detail::formatFloatLiteral(lhs + rhs);
  }
  if (opcode == "fsub") {
    return detail::formatFloatLiteral(lhs - rhs);
  }
  if (opcode == "fmul") {
    return detail::formatFloatLiteral(lhs * rhs);
  }
  if (opcode == "fdiv") {
    return rhs == 0.0 ? std::nullopt : std::optional<std::string>{detail::formatFloatLiteral(lhs / rhs)};
  }
  return std::nullopt;
}

std::optional<std::string> foldIntegerCompare(const std::string& opcode,
                                              const std::vector<std::string>& operands) {
  if (operands.size() != 2 || opcode.rfind("icmp.", 0) != 0) {
    return std::nullopt;
  }

  long long lhs = 0;
  long long rhs = 0;
  if (!detail::parseIntegerLiteral(operands[0], lhs) ||
      !detail::parseIntegerLiteral(operands[1], rhs)) {
    return std::nullopt;
  }

  const std::string predicate = opcode.substr(5);
  if (predicate == "eq") {
    return detail::formatBooleanLiteral(lhs == rhs);
  }
  if (predicate == "ne") {
    return detail::formatBooleanLiteral(lhs != rhs);
  }
  if (predicate == "slt") {
    return detail::formatBooleanLiteral(lhs < rhs);
  }
  if (predicate == "sle") {
    return detail::formatBooleanLiteral(lhs <= rhs);
  }
  if (predicate == "sgt") {
    return detail::formatBooleanLiteral(lhs > rhs);
  }
  if (predicate == "sge") {
    return detail::formatBooleanLiteral(lhs >= rhs);
  }
  return std::nullopt;
}

std::optional<std::string> foldFloatCompare(const std::string& opcode,
                                            const std::vector<std::string>& operands) {
  if (operands.size() != 2 || opcode.rfind("fcmp.", 0) != 0) {
    return std::nullopt;
  }

  double lhs = 0.0;
  double rhs = 0.0;
  if (!detail::parseFloatLiteral(operands[0], lhs) ||
      !detail::parseFloatLiteral(operands[1], rhs)) {
    return std::nullopt;
  }

  const std::string predicate = opcode.substr(5);
  if (predicate == "oeq") {
    return detail::formatBooleanLiteral(lhs == rhs);
  }
  if (predicate == "one") {
    return detail::formatBooleanLiteral(lhs != rhs);
  }
  if (predicate == "olt") {
    return detail::formatBooleanLiteral(lhs < rhs);
  }
  if (predicate == "ole") {
    return detail::formatBooleanLiteral(lhs <= rhs);
  }
  if (predicate == "ogt") {
    return detail::formatBooleanLiteral(lhs > rhs);
  }
  if (predicate == "oge") {
    return detail::formatBooleanLiteral(lhs >= rhs);
  }
  return std::nullopt;
}

std::optional<std::string> foldCast(const std::string& opcode,
                                    const std::vector<std::string>& operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto& value = operands[0];
  const auto& targetType = operands[1];
  if (opcode == "sitofp" || opcode == "uitofp") {
    long long integerValue = 0;
    if (targetType == "float" && detail::parseIntegerLiteral(value, integerValue)) {
      return detail::formatFloatLiteral(static_cast<double>(integerValue));
    }
    return std::nullopt;
  }
  if (opcode == "fptosi") {
    double floatValue = 0.0;
    if (detail::parseFloatLiteral(value, floatValue)) {
      return detail::formatIntegerLiteral(static_cast<long long>(floatValue));
    }
    return std::nullopt;
  }
  if (opcode == "zext" || opcode == "sext" || opcode == "trunc" || opcode == "cast") {
    long long integerValue = 0;
    if (!detail::parseIntegerLiteral(value, integerValue)) {
      return std::nullopt;
    }
    if (targetType == "bool") {
      return detail::formatBooleanLiteral(integerValue != 0);
    }
    return detail::formatIntegerLiteral(integerValue);
  }
  return std::nullopt;
}

std::optional<std::string> foldInstruction(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  if (!instruction.hasResult()) {
    return std::nullopt;
  }

  if (opcode == "neg" && operands.size() == 1) {
    long long value = 0;
    if (detail::parseIntegerLiteral(operands[0], value)) {
      return detail::formatIntegerLiteral(-value);
    }
  }
  if (opcode == "fneg" && operands.size() == 1) {
    double value = 0.0;
    if (detail::parseFloatLiteral(operands[0], value)) {
      return detail::formatFloatLiteral(-value);
    }
  }

  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
      opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
      opcode == "or" || opcode == "xor") {
    if (auto folded = foldIntegerBinary(opcode, operands); folded.has_value()) {
      return folded;
    }
  }
  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    if (auto folded = foldFloatBinary(opcode, operands); folded.has_value()) {
      return folded;
    }
  }
  if (auto folded = foldIntegerCompare(opcode, operands); folded.has_value()) {
    return folded;
  }
  if (auto folded = foldFloatCompare(opcode, operands); folded.has_value()) {
    return folded;
  }
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
      opcode == "sitofp" || opcode == "uitofp" || opcode == "fptosi") {
    return foldCast(opcode, operands);
  }

  return std::nullopt;
}

void foldFunction(Function& function) {
  std::unordered_map<std::string, std::string> constants;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewrittenInstructions;
    rewrittenInstructions.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      const auto operands = detail::rewriteOperands(instruction.operands(), constants);
      Instruction rewritten{instruction.opcode(), operands, instruction.result()};

      if (rewritten.opcode() == "br" && operands.size() == 3) {
        long long condition = 0;
        if (detail::parseIntegerLiteral(operands[0], condition)) {
          const std::string target = condition != 0 ? operands[1] : operands[2];
          rewrittenInstructions.emplace_back("br", std::vector<std::string>{target});
          continue;
        }
      }

      if (rewritten.hasResult()) {
        if (auto folded = foldInstruction(rewritten); folded.has_value()) {
          constants[rewritten.result()] = *folded;
          continue;
        }
        constants.erase(rewritten.result());
      }

      rewrittenInstructions.push_back(std::move(rewritten));
    }

    block.instructions() = std::move(rewrittenInstructions);
  }
}

}  // namespace

void ConstantFoldingPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    foldFunction(function);
  }
}

}  // namespace compiler::midend::pass
