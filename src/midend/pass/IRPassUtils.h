#pragma once

#include "../../../include/compiler/ir/core/Instruction.h"

#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compiler::midend::pass::detail {

inline bool parseIntegerLiteral(const std::string& text, long long& value) {
  if (text == "true") {
    value = 1;
    return true;
  }
  if (text == "false") {
    value = 0;
    return true;
  }

  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoll(text.c_str(), &end, 0);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

inline bool parseFloatLiteral(const std::string& text, double& value) {
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

inline bool isZero(const std::string& operand) {
  long long integerValue = 0;
  if (parseIntegerLiteral(operand, integerValue)) {
    return integerValue == 0;
  }
  double floatValue = 0.0;
  return parseFloatLiteral(operand, floatValue) && floatValue == 0.0;
}

inline bool isOne(const std::string& operand) {
  long long integerValue = 0;
  if (parseIntegerLiteral(operand, integerValue)) {
    return integerValue == 1;
  }
  double floatValue = 0.0;
  return parseFloatLiteral(operand, floatValue) && floatValue == 1.0;
}

inline std::string formatIntegerLiteral(long long value) {
  return std::to_string(value);
}

inline std::string formatBooleanLiteral(bool value) {
  return value ? "1" : "0";
}

inline std::string formatFloatLiteral(double value) {
  if (value == 0.0) {
    return "0.0";
  }
  std::ostringstream os;
  os << std::setprecision(17) << value;
  return os.str();
}

inline std::string resolveAlias(
    const std::string& operand, const std::unordered_map<std::string, std::string>& aliases) {
  std::string resolved = operand;
  std::unordered_set<std::string> seen;
  while (!resolved.empty() && resolved.front() == '%') {
    const auto found = aliases.find(resolved);
    if (found == aliases.end() || !seen.insert(resolved).second) {
      break;
    }
    resolved = found->second;
  }
  return resolved;
}

inline std::vector<std::string> rewriteOperands(
    const std::vector<std::string>& operands,
    const std::unordered_map<std::string, std::string>& aliases) {
  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());
  for (const auto& operand : operands) {
    rewritten.push_back(resolveAlias(operand, aliases));
  }
  return rewritten;
}

inline bool isSideEffectFree(const compiler::ir::core::Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  if (!instruction.hasResult()) {
    return false;
  }
  if (opcode == "call") {
    return false;
  }
  return opcode == "alloca" || opcode == "load" || opcode == "getelementptr" ||
         opcode == "getelementptr.linear" || opcode == "gep" || opcode == "gep.linear" ||
         opcode == "neg" || opcode == "fneg" || opcode == "cast" || opcode == "zext" ||
         opcode == "sext" || opcode == "trunc" || opcode == "sitofp" || opcode == "uitofp" ||
         opcode == "fptosi" || opcode == "add" || opcode == "sub" || opcode == "mul" ||
         opcode == "div" || opcode == "mod" || opcode == "sdiv" || opcode == "srem" ||
         opcode == "and" || opcode == "or" || opcode == "fadd" || opcode == "fsub" ||
         opcode == "fmul" || opcode == "fdiv" || opcode.rfind("icmp.", 0) == 0 ||
         opcode.rfind("fcmp.", 0) == 0;
}

}  // namespace compiler::midend::pass::detail
