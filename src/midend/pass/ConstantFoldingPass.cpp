#include "../../../include/compiler/midend/pass/ConstantFoldingPass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

using ValueMap = std::unordered_map<std::string, std::string>;

bool isSameValue(const std::string &lhs, const std::string &rhs) {
  return !lhs.empty() && lhs == rhs;
}

bool isLiteralValue(const std::string &value) {
  long long integerValue = 0;
  if (detail::parseIntegerLiteral(value, integerValue)) {
    return true;
  }
  double floatValue = 0.0;
  return detail::parseFloatLiteral(value, floatValue);
}

std::optional<std::string>
foldIntegerBinary(const std::string &opcode,
                  const std::vector<std::string> &operands) {
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
    return rhs == 0 ? std::nullopt
                    : std::optional<std::string>{
                          detail::formatIntegerLiteral(lhs / rhs)};
  }
  if (opcode == "mod" || opcode == "srem") {
    return rhs == 0 ? std::nullopt
                    : std::optional<std::string>{
                          detail::formatIntegerLiteral(lhs % rhs)};
  }
  if (opcode == "and") {
    return detail::formatIntegerLiteral(lhs & rhs);
  }
  if (opcode == "or") {
    return detail::formatIntegerLiteral(lhs | rhs);
  }
  return std::nullopt;
}

std::optional<std::string>
simplifyIntegerBinary(const std::string &opcode,
                      const std::vector<std::string> &operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto &lhs = operands[0];
  const auto &rhs = operands[1];
  if (opcode == "add") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (detail::isZero(lhs)) {
      return rhs;
    }
  }
  if (opcode == "sub") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (isSameValue(lhs, rhs)) {
      return "0";
    }
  }
  if (opcode == "mul") {
    if (detail::isZero(lhs) || detail::isZero(rhs)) {
      return "0";
    }
    if (detail::isOne(rhs)) {
      return lhs;
    }
    if (detail::isOne(lhs)) {
      return rhs;
    }
  }
  if ((opcode == "div" || opcode == "sdiv") && detail::isOne(rhs)) {
    return lhs;
  }
  if ((opcode == "mod" || opcode == "srem") && detail::isOne(rhs)) {
    return "0";
  }
  if (opcode == "and") {
    if (detail::isZero(lhs) || detail::isZero(rhs)) {
      return "0";
    }
    if (isSameValue(lhs, rhs)) {
      return lhs;
    }
  }
  if (opcode == "or") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (detail::isZero(lhs)) {
      return rhs;
    }
    if (isSameValue(lhs, rhs)) {
      return lhs;
    }
  }
  return std::nullopt;
}

std::optional<std::string>
foldFloatBinary(const std::string &opcode,
                const std::vector<std::string> &operands) {
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
    return rhs == 0.0 ? std::nullopt
                      : std::optional<std::string>{
                            detail::formatFloatLiteral(lhs / rhs)};
  }
  return std::nullopt;
}

std::optional<std::string>
simplifyFloatBinary(const std::string &opcode,
                    const std::vector<std::string> &operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto &lhs = operands[0];
  const auto &rhs = operands[1];
  if (opcode == "fmul") {
    if (detail::isOne(rhs)) {
      return lhs;
    }
    if (detail::isOne(lhs)) {
      return rhs;
    }
  }
  if (opcode == "fdiv" && detail::isOne(rhs)) {
    return lhs;
  }
  return std::nullopt;
}

std::optional<std::string>
foldIntegerCompare(const std::string &opcode,
                   const std::vector<std::string> &operands) {
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

std::optional<std::string>
simplifyIntegerCompare(const std::string &opcode,
                       const std::vector<std::string> &operands) {
  if (operands.size() != 2 || opcode.rfind("icmp.", 0) != 0 ||
      !isSameValue(operands[0], operands[1])) {
    return std::nullopt;
  }

  const std::string predicate = opcode.substr(5);
  if (predicate == "eq" || predicate == "sle" || predicate == "sge") {
    return detail::formatBooleanLiteral(true);
  }
  if (predicate == "ne" || predicate == "slt" || predicate == "sgt") {
    return detail::formatBooleanLiteral(false);
  }
  return std::nullopt;
}

std::optional<std::string>
foldFloatCompare(const std::string &opcode,
                 const std::vector<std::string> &operands) {
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

std::optional<std::string> foldCast(const std::string &opcode,
                                    const std::vector<std::string> &operands) {
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto &value = operands[0];
  const auto &targetType = operands[1];
  if (opcode == "sitofp" || opcode == "uitofp") {
    long long integerValue = 0;
    if (targetType == "float" &&
        detail::parseIntegerLiteral(value, integerValue)) {
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
  if (opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
      opcode == "cast") {
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

std::optional<std::string> foldInstruction(const Instruction &instruction) {
  const auto &opcode = instruction.opcode();
  const auto &operands = instruction.operands();
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

  if (opcode == "add" || opcode == "sub" || opcode == "mul" ||
      opcode == "div" || opcode == "mod" || opcode == "sdiv" ||
      opcode == "srem" || opcode == "and" || opcode == "or") {
    if (auto folded = foldIntegerBinary(opcode, operands); folded.has_value()) {
      return folded;
    }
    if (auto simplified = simplifyIntegerBinary(opcode, operands);
        simplified.has_value()) {
      return simplified;
    }
  }
  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" ||
      opcode == "fdiv") {
    if (auto folded = foldFloatBinary(opcode, operands); folded.has_value()) {
      return folded;
    }
    if (auto simplified = simplifyFloatBinary(opcode, operands);
        simplified.has_value()) {
      return simplified;
    }
  }
  if (auto folded = foldIntegerCompare(opcode, operands); folded.has_value()) {
    return folded;
  }
  if (auto simplified = simplifyIntegerCompare(opcode, operands);
      simplified.has_value()) {
    return simplified;
  }
  if (auto folded = foldFloatCompare(opcode, operands); folded.has_value()) {
    return folded;
  }
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" ||
      opcode == "trunc" || opcode == "sitofp" || opcode == "uitofp" ||
      opcode == "fptosi") {
    return foldCast(opcode, operands);
  }

  return std::nullopt;
}

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto &operands = terminator->operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

std::unordered_map<std::string, std::vector<std::string>>
computePredecessors(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto &block : function.blocks()) {
    predecessors.emplace(block.name(), std::vector<std::string>{});
  }
  for (const auto &block : function.blocks()) {
    for (const auto &target : branchTargets(block)) {
      predecessors[target].push_back(block.name());
    }
  }
  return predecessors;
}

ValueMap
meetConstants(const std::vector<std::string> &predecessors,
              const std::unordered_map<std::string, ValueMap> &outConstants) {
  if (predecessors.empty()) {
    return {};
  }

  ValueMap result;
  bool initialized = false;
  for (const auto &predecessor : predecessors) {
    const auto found = outConstants.find(predecessor);
    if (found == outConstants.end()) {
      continue;
    }
    if (!initialized) {
      result = found->second;
      initialized = true;
      continue;
    }
    for (auto it = result.begin(); it != result.end();) {
      const auto predValue = found->second.find(it->first);
      if (predValue == found->second.end() || predValue->second != it->second) {
        it = result.erase(it);
      } else {
        ++it;
      }
    }
  }
  return result;
}

ValueMap transferConstants(const BasicBlock &block,
                           const ValueMap &inConstants) {
  ValueMap constants = inConstants;
  for (const auto &instruction : block.instructions()) {
    const auto operands =
        detail::rewriteOperands(instruction.operands(), constants);
    Instruction rewritten{instruction.opcode(), operands, instruction.result()};
    if (!rewritten.hasResult()) {
      continue;
    }
    if (auto folded = foldInstruction(rewritten);
        folded.has_value() && isLiteralValue(*folded)) {
      constants[rewritten.result()] = *folded;
    } else {
      constants.erase(rewritten.result());
    }
  }
  return constants;
}

std::unordered_map<std::string, ValueMap>
solveBlockEntryConstants(const Function &function) {
  const auto predecessors = computePredecessors(function);
  std::unordered_map<std::string, ValueMap> inConstants;
  std::unordered_map<std::string, ValueMap> outConstants;
  for (const auto &block : function.blocks()) {
    inConstants.emplace(block.name(), ValueMap{});
    outConstants.emplace(block.name(), ValueMap{});
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &block : function.blocks()) {
      ValueMap incoming;
      if (&block != &function.blocks().front()) {
        const auto predIt = predecessors.find(block.name());
        if (predIt != predecessors.end()) {
          incoming = meetConstants(predIt->second, outConstants);
        }
      }
      if (incoming != inConstants[block.name()]) {
        inConstants[block.name()] = incoming;
        changed = true;
      }

      const auto outgoing = transferConstants(block, inConstants[block.name()]);
      if (outgoing != outConstants[block.name()]) {
        outConstants[block.name()] = outgoing;
        changed = true;
      }
    }
  }
  return inConstants;
}

void foldBlock(BasicBlock &block, const ValueMap &inConstants) {
  ValueMap replacements = inConstants;
  std::vector<Instruction> rewrittenInstructions;
  rewrittenInstructions.reserve(block.instructions().size());

  for (const auto &instruction : block.instructions()) {
    const auto operands =
        detail::rewriteOperands(instruction.operands(), replacements);
    Instruction rewritten{instruction.opcode(), operands, instruction.result()};

    if (rewritten.opcode() == "br" && operands.size() == 3) {
      long long condition = 0;
      if (detail::parseIntegerLiteral(operands[0], condition)) {
        const std::string target = condition != 0 ? operands[1] : operands[2];
        rewrittenInstructions.emplace_back("br",
                                           std::vector<std::string>{target});
        continue;
      }
    }

    if (rewritten.hasResult()) {
      if (auto folded = foldInstruction(rewritten); folded.has_value()) {
        replacements[rewritten.result()] = *folded;
        continue;
      }
      replacements.erase(rewritten.result());
    }
    rewrittenInstructions.push_back(std::move(rewritten));
  }

  block.instructions() = std::move(rewrittenInstructions);
}

void foldFunction(Function &function) {
  const auto inConstants = solveBlockEntryConstants(function);
  for (auto &block : function.blocks()) {
    const auto found = inConstants.find(block.name());
    foldBlock(block, found == inConstants.end() ? ValueMap{} : found->second);
  }
}

} // namespace

bool ConstantFoldingPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    foldFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
