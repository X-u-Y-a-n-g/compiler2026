#include "../../../include/compiler/midend/pass/SparseConditionalConstantPropagationPass.h"

#include <optional>
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

enum class LatticeKind {
  Unknown,
  Constant,
  Overdefined,
};

struct LatticeValue {
  LatticeKind kind = LatticeKind::Unknown;
  std::string constant;

  static LatticeValue unknown() { return {}; }
  static LatticeValue constantValue(std::string value) {
    return LatticeValue{LatticeKind::Constant, std::move(value)};
  }
  static LatticeValue overdefined() { return LatticeValue{LatticeKind::Overdefined, {}}; }

  [[nodiscard]] bool isUnknown() const { return kind == LatticeKind::Unknown; }
  [[nodiscard]] bool isConstant() const { return kind == LatticeKind::Constant; }
  [[nodiscard]] bool isOverdefined() const { return kind == LatticeKind::Overdefined; }
};

bool operator==(const LatticeValue& lhs, const LatticeValue& rhs) {
  return lhs.kind == rhs.kind && lhs.constant == rhs.constant;
}

LatticeValue mergeValues(const LatticeValue& lhs, const LatticeValue& rhs) {
  if (lhs.isUnknown()) {
    return rhs;
  }
  if (rhs.isUnknown()) {
    return lhs;
  }
  if (lhs.isOverdefined() || rhs.isOverdefined()) {
    return LatticeValue::overdefined();
  }
  if (lhs.constant == rhs.constant) {
    return lhs;
  }
  return LatticeValue::overdefined();
}

bool isValueReference(const std::string& operand) {
  return !operand.empty() && operand.front() == '%';
}

bool isLiteralConstant(const std::string& operand) {
  long long integerValue = 0;
  if (detail::parseIntegerLiteral(operand, integerValue)) {
    return true;
  }
  double floatValue = 0.0;
  return detail::parseFloatLiteral(operand, floatValue);
}

LatticeValue valueOf(const std::string& operand,
                     const std::unordered_map<std::string, LatticeValue>& values) {
  if (isValueReference(operand)) {
    const auto found = values.find(operand);
    return found == values.end() ? LatticeValue::unknown() : found->second;
  }
  if (!operand.empty() && operand.front() == '@') {
    return LatticeValue::overdefined();
  }
  if (isLiteralConstant(operand)) {
    return LatticeValue::constantValue(operand);
  }
  return LatticeValue::overdefined();
}

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
    return rhs == 0 ? std::nullopt
                    : std::optional<std::string>{detail::formatIntegerLiteral(lhs / rhs)};
  }
  if (opcode == "mod" || opcode == "srem") {
    return rhs == 0 ? std::nullopt
                    : std::optional<std::string>{detail::formatIntegerLiteral(lhs % rhs)};
  }
  if (opcode == "and") {
    return detail::formatIntegerLiteral(lhs & rhs);
  }
  if (opcode == "or") {
    return detail::formatIntegerLiteral(lhs | rhs);
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
    return rhs == 0.0 ? std::nullopt
                      : std::optional<std::string>{detail::formatFloatLiteral(lhs / rhs)};
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

std::optional<std::string> foldInstruction(const std::string& opcode,
                                           const std::vector<std::string>& operands) {
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
      opcode == "or") {
    return foldIntegerBinary(opcode, operands);
  }
  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    return foldFloatBinary(opcode, operands);
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

bool updateValue(std::unordered_map<std::string, LatticeValue>& values, const std::string& name,
                 const LatticeValue& incoming) {
  if (name.empty()) {
    return false;
  }
  const auto found = values.find(name);
  const auto old = found == values.end() ? LatticeValue::unknown() : found->second;
  const auto merged = mergeValues(old, incoming);
  if (merged == old) {
    return false;
  }
  values[name] = merged;
  return true;
}

struct CFG {
  std::unordered_map<std::string, std::size_t> indexByName;
  std::vector<std::string> names;
};

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.names.reserve(function.blocks().size());
  for (const auto& block : function.blocks()) {
    cfg.indexByName.emplace(block.name(), cfg.names.size());
    cfg.names.push_back(block.name());
  }
  return cfg;
}

std::string edgeKey(const std::string& from, const std::string& to) {
  return from + "\n" + to;
}

bool isExecutableEdge(const std::unordered_set<std::string>& executableEdges,
                      const std::string& from, const std::string& to) {
  return executableEdges.find(edgeKey(from, to)) != executableEdges.end();
}

bool markEdge(const CFG& cfg, std::vector<bool>& reachable,
              std::unordered_set<std::string>& executableEdges, const std::string& from,
              const std::string& to) {
  const auto target = cfg.indexByName.find(to);
  if (target == cfg.indexByName.end()) {
    return false;
  }

  bool changed = false;
  changed |= executableEdges.insert(edgeKey(from, to)).second;
  if (!reachable[target->second]) {
    reachable[target->second] = true;
    changed = true;
  }
  return changed;
}

LatticeValue evaluatePhi(const Instruction& instruction, const std::string& blockName,
                         const std::unordered_set<std::string>& executableEdges,
                         const std::unordered_map<std::string, LatticeValue>& values) {
  LatticeValue result = LatticeValue::unknown();
  const auto& operands = instruction.operands();
  for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
    const auto& incomingValue = operands[i];
    const auto& incomingBlock = operands[i + 1];
    if (!isExecutableEdge(executableEdges, incomingBlock, blockName)) {
      continue;
    }
    result = mergeValues(result, valueOf(incomingValue, values));
  }
  return result;
}

LatticeValue evaluateInstruction(const Instruction& instruction,
                                 const std::unordered_map<std::string, LatticeValue>& values) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();

  std::vector<std::size_t> valueOperandIndexes;
  if (opcode == "neg" || opcode == "fneg") {
    valueOperandIndexes = {0};
  } else if (opcode == "cast" || opcode == "zext" || opcode == "sext" ||
             opcode == "trunc" || opcode == "sitofp" || opcode == "uitofp" ||
             opcode == "fptosi") {
    valueOperandIndexes = {0};
  } else if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
             opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
             opcode == "or" || opcode == "fadd" || opcode == "fsub" || opcode == "fmul" ||
             opcode == "fdiv" || opcode.rfind("icmp.", 0) == 0 ||
             opcode.rfind("fcmp.", 0) == 0) {
    valueOperandIndexes = {0, 1};
  } else {
    return LatticeValue::overdefined();
  }

  std::vector<std::string> foldedOperands = operands;
  for (const auto index : valueOperandIndexes) {
    if (index >= operands.size()) {
      return LatticeValue::overdefined();
    }
    const auto value = valueOf(operands[index], values);
    if (value.isUnknown()) {
      return LatticeValue::unknown();
    }
    if (value.isOverdefined()) {
      return LatticeValue::overdefined();
    }
    foldedOperands[index] = value.constant;
  }

  if (auto folded = foldInstruction(opcode, foldedOperands); folded.has_value()) {
    return LatticeValue::constantValue(*folded);
  }
  return LatticeValue::overdefined();
}

bool visitTerminator(const CFG& cfg, std::vector<bool>& reachable,
                     std::unordered_set<std::string>& executableEdges,
                     const std::unordered_map<std::string, LatticeValue>& values,
                     const std::string& blockName, const Instruction& instruction) {
  if (instruction.opcode() != "br") {
    return false;
  }

  const auto& operands = instruction.operands();
  if (operands.size() == 1) {
    return markEdge(cfg, reachable, executableEdges, blockName, operands[0]);
  }
  if (operands.size() != 3) {
    return false;
  }

  const auto condition = valueOf(operands[0], values);
  if (condition.isUnknown()) {
    return false;
  }
  if (condition.isConstant()) {
    long long integerValue = 0;
    if (detail::parseIntegerLiteral(condition.constant, integerValue)) {
      return markEdge(cfg, reachable, executableEdges, blockName,
                      integerValue != 0 ? operands[1] : operands[2]);
    }
  }

  bool changed = false;
  changed |= markEdge(cfg, reachable, executableEdges, blockName, operands[1]);
  changed |= markEdge(cfg, reachable, executableEdges, blockName, operands[2]);
  return changed;
}

std::unordered_map<std::string, std::string> analyzeConstants(Function& function) {
  std::unordered_map<std::string, LatticeValue> values;
  for (const auto& parameter : function.parameters()) {
    values[parameter.name] = LatticeValue::overdefined();
  }

  const CFG cfg = buildCFG(function);
  std::vector<bool> reachable(function.blocks().size(), false);
  std::unordered_set<std::string> executableEdges;
  if (!reachable.empty()) {
    reachable[0] = true;
  }

  bool changed = false;
  do {
    changed = false;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size(); ++blockIndex) {
      if (!reachable[blockIndex]) {
        continue;
      }

      const auto& block = function.blocks()[blockIndex];
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() == "phi" && instruction.hasResult()) {
          changed |= updateValue(values, instruction.result(),
                                 evaluatePhi(instruction, block.name(), executableEdges, values));
          continue;
        }

        if (instruction.hasResult()) {
          changed |= updateValue(values, instruction.result(),
                                 evaluateInstruction(instruction, values));
        }

        if (instruction.isTerminator()) {
          changed |= visitTerminator(cfg, reachable, executableEdges, values, block.name(),
                                     instruction);
          break;
        }
      }
    }
  } while (changed);

  std::unordered_map<std::string, std::string> constants;
  for (const auto& [name, value] : values) {
    if (value.isConstant()) {
      constants.emplace(name, value.constant);
    }
  }
  return constants;
}

std::string rewriteOperand(const std::string& operand,
                           const std::unordered_map<std::string, std::string>& constants) {
  const auto found = constants.find(operand);
  return found == constants.end() ? operand : found->second;
}

std::vector<std::string> rewriteOperands(const Instruction& instruction,
                                         const std::unordered_map<std::string, std::string>& constants) {
  const auto& operands = instruction.operands();
  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());

  if (instruction.opcode() == "phi") {
    if (!operands.empty()) {
      rewritten.push_back(operands.front());
    }
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      rewritten.push_back(rewriteOperand(operands[i], constants));
      rewritten.push_back(operands[i + 1]);
    }
    return rewritten;
  }

  if (instruction.opcode() == "br") {
    if (operands.size() == 1) {
      return operands;
    }
    if (operands.size() == 3) {
      const std::string condition = rewriteOperand(operands[0], constants);
      long long integerValue = 0;
      if (detail::parseIntegerLiteral(condition, integerValue)) {
        return {integerValue != 0 ? operands[1] : operands[2]};
      }
      return {condition, operands[1], operands[2]};
    }
  }

  for (const auto& operand : operands) {
    rewritten.push_back(rewriteOperand(operand, constants));
  }
  return rewritten;
}

void rewriteFunction(Function& function,
                     const std::unordered_map<std::string, std::string>& constants) {
  if (constants.empty()) {
    return;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && constants.find(instruction.result()) != constants.end()) {
        continue;
      }
      rewritten.emplace_back(instruction.opcode(), rewriteOperands(instruction, constants),
                             instruction.result());
    }

    block.instructions() = std::move(rewritten);
  }
}

void runSccp(Function& function) {
  rewriteFunction(function, analyzeConstants(function));
}

}  // namespace

void SparseConditionalConstantPropagationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    runSccp(function);
  }
}

}  // namespace compiler::midend::pass
