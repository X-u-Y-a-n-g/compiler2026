#include "../../../include/compiler/midend/pass/ConstantFoldingPass.h"

#include <cstdint>
#include <limits>
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

std::optional<std::int32_t> parseI32(const std::string& operand) {
  long long value = 0;
  if (!detail::parseIntegerLiteral(operand, value) ||
      value < std::numeric_limits<std::int32_t>::min() ||
      value > std::numeric_limits<std::int32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>(value);
}

std::optional<std::string> foldIntegerIdentity(const std::string& opcode,
                                               const std::vector<std::string>& operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto lhs = parseI32(operands[0]);
  const auto rhs = parseI32(operands[1]);
  if (opcode == "add") {
    if (lhs == 0) return operands[1];
    if (rhs == 0) return operands[0];
  }
  if (opcode == "sub" && rhs == 0) return operands[0];
  if (opcode == "mul") {
    if (lhs == 0 || rhs == 0) return "0";
    if (lhs == 1) return operands[1];
    if (rhs == 1) return operands[0];
  }
  if ((opcode == "div" || opcode == "sdiv") && rhs == 1) return operands[0];
  if ((opcode == "mod" || opcode == "srem") && rhs == 1) return "0";
  if (opcode == "and") {
    if (lhs == 0 || rhs == 0) return "0";
    if (lhs == -1) return operands[1];
    if (rhs == -1) return operands[0];
  }
  if (opcode == "or" || opcode == "xor") {
    if (lhs == 0) return operands[1];
    if (rhs == 0) return operands[0];
  }
  if ((opcode == "shl" || opcode == "ashr" || opcode == "lshr") && rhs == 0) {
    return operands[0];
  }
  return std::nullopt;
}

std::optional<std::string> foldIntegerCompare(const std::string& opcode,
                                              const std::vector<std::string>& operands) {
  if (operands.size() != 2 || opcode.rfind("icmp.", 0) != 0) {
    return std::nullopt;
  }

  const auto lhs = parseI32(operands[0]);
  const auto rhs = parseI32(operands[1]);
  if (!lhs.has_value() || !rhs.has_value()) {
    return std::nullopt;
  }

  const std::string predicate = opcode.substr(5);
  if (predicate == "eq") {
    return detail::formatBooleanLiteral(*lhs == *rhs);
  }
  if (predicate == "ne") {
    return detail::formatBooleanLiteral(*lhs != *rhs);
  }
  if (predicate == "slt") {
    return detail::formatBooleanLiteral(*lhs < *rhs);
  }
  if (predicate == "sle") {
    return detail::formatBooleanLiteral(*lhs <= *rhs);
  }
  if (predicate == "sgt") {
    return detail::formatBooleanLiteral(*lhs > *rhs);
  }
  if (predicate == "sge") {
    return detail::formatBooleanLiteral(*lhs >= *rhs);
  }
  return std::nullopt;
}

std::optional<std::string> foldInstruction(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  if (!instruction.hasResult()) {
    return std::nullopt;
  }

  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
      opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
      opcode == "or" || opcode == "xor" || opcode == "shl" || opcode == "ashr" ||
      opcode == "lshr") {
    if (auto folded = foldIntegerIdentity(opcode, operands); folded.has_value()) {
      return folded;
    }
  }
  if (opcode.rfind("icmp.", 0) == 0 && operands.size() == 2 && operands[0] == operands[1]) {
    const std::string predicate = opcode.substr(5);
    if (predicate == "eq" || predicate == "sle" || predicate == "sge") return "1";
    if (predicate == "ne" || predicate == "slt" || predicate == "sgt") return "0";
  }
  if (auto folded = foldIntegerCompare(opcode, operands); folded.has_value()) {
    return folded;
  }

  return std::nullopt;
}

void foldFunction(Function& function) {
  std::unordered_map<std::string, std::string> constants;

  // Discover folded definitions to a fixed point first.  Loop-header phi nodes
  // may use a value defined in a later latch block, so deleting definitions
  // during a single layout-order traversal would leave such uses unresolved.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult()) {
          continue;
        }
        const auto operands = detail::rewriteOperands(instruction.operands(), constants);
        const Instruction rewritten{instruction.opcode(), operands, instruction.result()};
        const auto folded = foldInstruction(rewritten);
        if (!folded.has_value()) {
          continue;
        }
        const auto found = constants.find(instruction.result());
        if (found == constants.end() || found->second != *folded) {
          constants[instruction.result()] = *folded;
          changed = true;
        }
      }
    }
  }

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

      if (rewritten.hasResult() && constants.find(rewritten.result()) != constants.end()) {
        continue;
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
