#include "../../../include/compiler/midend/pass/SparseConditionalConstantPropagationPass.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

enum class LatticeKind { Unknown, Constant, Overdefined };

struct LatticeValue {
  LatticeKind kind = LatticeKind::Unknown;
  long long constant = 0;

  static LatticeValue unknown() { return {}; }
  static LatticeValue constantValue(long long value) {
    return {LatticeKind::Constant, value};
  }
  static LatticeValue overdefined() { return {LatticeKind::Overdefined, 0}; }
};

bool operator==(const LatticeValue &lhs, const LatticeValue &rhs) {
  return lhs.kind == rhs.kind &&
         (lhs.kind != LatticeKind::Constant || lhs.constant == rhs.constant);
}

bool isLocalValue(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

std::string formatConstant(long long value) {
  return detail::formatIntegerLiteral(value);
}

bool isCheapImmediate(long long value) { return value >= -2048 && value <= 2047; }

LatticeValue meet(const LatticeValue &lhs, const LatticeValue &rhs) {
  if (lhs.kind == LatticeKind::Unknown) {
    return rhs;
  }
  if (rhs.kind == LatticeKind::Unknown) {
    return lhs;
  }
  if (lhs.kind == LatticeKind::Overdefined ||
      rhs.kind == LatticeKind::Overdefined) {
    return LatticeValue::overdefined();
  }
  if (lhs.constant == rhs.constant) {
    return lhs;
  }
  return LatticeValue::overdefined();
}

std::unordered_map<std::string, const BasicBlock *>
blockMap(const Function &function) {
  std::unordered_map<std::string, const BasicBlock *> blocks;
  for (const auto &block : function.blocks()) {
    blocks.emplace(block.name(), &block);
  }
  return blocks;
}

std::string edgeKey(const std::string &from, const std::string &to) {
  return from + "\n" + to;
}

LatticeValue
operandValue(const std::string &operand,
             const std::unordered_map<std::string, LatticeValue> &values) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(operand, literal)) {
    return LatticeValue::constantValue(literal);
  }
  if (!isLocalValue(operand)) {
    return LatticeValue::overdefined();
  }
  const auto found = values.find(operand);
  return found == values.end() ? LatticeValue::unknown() : found->second;
}

LatticeValue evaluateIntegerBinary(const std::string &opcode, long long lhs,
                                   long long rhs) {
  if (opcode == "add") {
    return LatticeValue::constantValue(lhs + rhs);
  }
  if (opcode == "sub") {
    return LatticeValue::constantValue(lhs - rhs);
  }
  if (opcode == "mul") {
    return LatticeValue::constantValue(lhs * rhs);
  }
  if ((opcode == "div" || opcode == "sdiv") && rhs != 0) {
    return LatticeValue::constantValue(lhs / rhs);
  }
  if ((opcode == "mod" || opcode == "srem") && rhs != 0) {
    return LatticeValue::constantValue(lhs % rhs);
  }
  if (opcode == "and") {
    return LatticeValue::constantValue(lhs & rhs);
  }
  if (opcode == "or") {
    return LatticeValue::constantValue(lhs | rhs);
  }
  return LatticeValue::overdefined();
}

LatticeValue evaluateIntegerCompare(const std::string &opcode, long long lhs,
                                    long long rhs) {
  if (opcode.rfind("icmp.", 0) != 0) {
    return LatticeValue::overdefined();
  }
  const std::string predicate = opcode.substr(5);
  if (predicate == "eq") {
    return LatticeValue::constantValue(lhs == rhs ? 1 : 0);
  }
  if (predicate == "ne") {
    return LatticeValue::constantValue(lhs != rhs ? 1 : 0);
  }
  if (predicate == "slt") {
    return LatticeValue::constantValue(lhs < rhs ? 1 : 0);
  }
  if (predicate == "sle") {
    return LatticeValue::constantValue(lhs <= rhs ? 1 : 0);
  }
  if (predicate == "sgt") {
    return LatticeValue::constantValue(lhs > rhs ? 1 : 0);
  }
  if (predicate == "sge") {
    return LatticeValue::constantValue(lhs >= rhs ? 1 : 0);
  }
  return LatticeValue::overdefined();
}

LatticeValue
evaluateInstruction(const Instruction &instruction,
                    const std::string &blockName,
                    const std::unordered_map<std::string, LatticeValue> &values,
                    const std::unordered_set<std::string> &executableEdges) {
  const auto &opcode = instruction.opcode();
  const auto &operands = instruction.operands();

  if (opcode == "phi") {
    LatticeValue result = LatticeValue::unknown();
    bool sawIncoming = false;
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      const auto &incomingBlock = operands[i + 1];
      if (executableEdges.find(edgeKey(incomingBlock, blockName)) ==
          executableEdges.end()) {
        continue;
      }
      sawIncoming = true;
      result = meet(result, operandValue(operands[i], values));
      if (result.kind == LatticeKind::Overdefined) {
        break;
      }
    }
    return sawIncoming ? result : LatticeValue::unknown();
  }

  if (opcode == "neg" && operands.size() == 1) {
    const auto value = operandValue(operands[0], values);
    if (value.kind == LatticeKind::Constant) {
      return LatticeValue::constantValue(-value.constant);
    }
    return value.kind == LatticeKind::Unknown ? LatticeValue::unknown()
                                              : LatticeValue::overdefined();
  }

  if ((opcode == "add" || opcode == "sub" || opcode == "mul" ||
       opcode == "div" || opcode == "mod" || opcode == "sdiv" ||
       opcode == "srem" || opcode == "and" || opcode == "or") &&
      operands.size() == 2) {
    const auto lhs = operandValue(operands[0], values);
    const auto rhs = operandValue(operands[1], values);
    if (lhs.kind == LatticeKind::Constant &&
        rhs.kind == LatticeKind::Constant) {
      return evaluateIntegerBinary(opcode, lhs.constant, rhs.constant);
    }
    if (lhs.kind == LatticeKind::Overdefined ||
        rhs.kind == LatticeKind::Overdefined) {
      return LatticeValue::overdefined();
    }
    return LatticeValue::unknown();
  }

  if (opcode.rfind("icmp.", 0) == 0 && operands.size() == 2) {
    const auto lhs = operandValue(operands[0], values);
    const auto rhs = operandValue(operands[1], values);
    if (lhs.kind == LatticeKind::Constant &&
        rhs.kind == LatticeKind::Constant) {
      return evaluateIntegerCompare(opcode, lhs.constant, rhs.constant);
    }
    if (lhs.kind == LatticeKind::Overdefined ||
        rhs.kind == LatticeKind::Overdefined) {
      return LatticeValue::overdefined();
    }
    return LatticeValue::unknown();
  }

  if ((opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
       opcode == "cast") &&
      operands.size() == 2) {
    const auto value = operandValue(operands[0], values);
    if (value.kind == LatticeKind::Constant) {
      return LatticeValue::constantValue(value.constant);
    }
    return value.kind == LatticeKind::Unknown ? LatticeValue::unknown()
                                              : LatticeValue::overdefined();
  }

  return instruction.hasResult() ? LatticeValue::overdefined()
                                 : LatticeValue::unknown();
}

bool updateValue(std::unordered_map<std::string, LatticeValue> &values,
                 const std::string &name, const LatticeValue &next) {
  const auto current = values.find(name);
  if (current == values.end()) {
    values.emplace(name, next);
    return next.kind != LatticeKind::Unknown;
  }
  const auto merged = meet(current->second, next);
  if (merged == current->second) {
    return false;
  }
  current->second = merged;
  return true;
}

bool markEdge(std::unordered_set<std::string> &reachableBlocks,
              std::unordered_set<std::string> &executableEdges,
              const std::unordered_map<std::string, const BasicBlock *> &blocks,
              const std::string &from, const std::string &to) {
  if (blocks.find(to) == blocks.end()) {
    return false;
  }
  bool changed = false;
  changed |= executableEdges.insert(edgeKey(from, to)).second;
  changed |= reachableBlocks.insert(to).second;
  return changed;
}

void solveFunction(const Function &function,
                   std::unordered_map<std::string, LatticeValue> &values,
                   std::unordered_set<std::string> &reachableBlocks,
                   std::unordered_set<std::string> &executableEdges) {
  if (function.blocks().empty()) {
    return;
  }

  const auto blocks = blockMap(function);
  reachableBlocks.insert(function.blocks().front().name());
  for (const auto &parameter : function.parameters()) {
    values[parameter.name] = LatticeValue::overdefined();
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &block : function.blocks()) {
      if (reachableBlocks.find(block.name()) == reachableBlocks.end()) {
        continue;
      }

      for (const auto &instruction : block.instructions()) {
        if (instruction.hasResult()) {
          const auto next = evaluateInstruction(instruction, block.name(),
                                                values, executableEdges);
          changed |= updateValue(values, instruction.result(), next);
        }
      }

      const auto *terminator = block.terminator();
      if (terminator == nullptr || terminator->opcode() != "br") {
        continue;
      }
      const auto &operands = terminator->operands();
      if (operands.size() == 1) {
        changed |= markEdge(reachableBlocks, executableEdges, blocks,
                            block.name(), operands[0]);
      } else if (operands.size() == 3) {
        const auto condition = operandValue(operands[0], values);
        if (condition.kind == LatticeKind::Constant) {
          changed |=
              markEdge(reachableBlocks, executableEdges, blocks, block.name(),
                       condition.constant != 0 ? operands[1] : operands[2]);
        } else if (condition.kind == LatticeKind::Overdefined) {
          changed |= markEdge(reachableBlocks, executableEdges, blocks,
                              block.name(), operands[1]);
          changed |= markEdge(reachableBlocks, executableEdges, blocks,
                              block.name(), operands[2]);
        }
      }
    }
  }
}

std::string
rewriteOperand(const std::string &operand,
               const std::unordered_map<std::string, LatticeValue> &values,
               const std::unordered_set<std::string> &rewriteableConstants) {
  if (!isLocalValue(operand)) {
    return operand;
  }
  if (rewriteableConstants.find(operand) == rewriteableConstants.end()) {
    return operand;
  }
  const auto found = values.find(operand);
  if (found != values.end() && found->second.kind == LatticeKind::Constant) {
    return formatConstant(found->second.constant);
  }
  return operand;
}

std::vector<std::string>
rewriteOperands(
    const Instruction &instruction,
    const std::unordered_map<std::string, LatticeValue> &values,
    const std::unordered_set<std::string> &rewriteableConstants) {
  const auto &operands = instruction.operands();
  std::vector<std::string> rewritten;
  rewritten.reserve(operands.size());

  if (instruction.opcode() == "phi") {
    if (!operands.empty()) {
      rewritten.push_back(operands.front());
    }
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      rewritten.push_back(
          rewriteOperand(operands[i], values, rewriteableConstants));
      rewritten.push_back(operands[i + 1]);
    }
    return rewritten;
  }

  if (instruction.opcode() == "call") {
    for (std::size_t i = 0; i < operands.size(); ++i) {
      rewritten.push_back(i == 0 ? operands[i]
                                 : rewriteOperand(operands[i], values,
                                                  rewriteableConstants));
    }
    return rewritten;
  }

  for (const auto &operand : operands) {
    rewritten.push_back(rewriteOperand(operand, values, rewriteableConstants));
  }
  return rewritten;
}

std::unordered_map<std::string, int> computeUseCounts(const Function &function) {
  std::unordered_map<std::string, int> uses;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      if (instruction.opcode() == "phi") {
        for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
          if (isLocalValue(operands[i])) {
            ++uses[operands[i]];
          }
        }
        continue;
      }
      if (instruction.opcode() == "call") {
        for (std::size_t i = 1; i < operands.size(); ++i) {
          if (isLocalValue(operands[i])) {
            ++uses[operands[i]];
          }
        }
        continue;
      }
      for (const auto &operand : operands) {
        if (isLocalValue(operand)) {
          ++uses[operand];
        }
      }
    }
  }
  return uses;
}

std::unordered_set<std::string> computeRewriteableConstants(
    const Function &function,
    const std::unordered_map<std::string, LatticeValue> &values) {
  const auto uses = computeUseCounts(function);
  std::unordered_set<std::string> rewriteable;
  for (const auto &[name, value] : values) {
    if (value.kind != LatticeKind::Constant) {
      continue;
    }
    const auto useIt = uses.find(name);
    const int useCount = useIt == uses.end() ? 0 : useIt->second;
    if (useCount <= 1 || isCheapImmediate(value.constant)) {
      rewriteable.insert(name);
    }
  }
  return rewriteable;
}

bool constantValueForOperand(
    const std::string &operand,
    const std::unordered_map<std::string, LatticeValue> &values,
    long long &constant) {
  if (detail::parseIntegerLiteral(operand, constant)) {
    return true;
  }
  if (!isLocalValue(operand)) {
    return false;
  }
  const auto found = values.find(operand);
  if (found == values.end() || found->second.kind != LatticeKind::Constant) {
    return false;
  }
  constant = found->second.constant;
  return true;
}

bool rewriteFunction(
    Function &function,
    const std::unordered_map<std::string, LatticeValue> &values,
    const std::unordered_set<std::string> &reachableBlocks) {
  bool changed = false;
  const auto rewriteableConstants = computeRewriteableConstants(function, values);
  const auto oldBlockCount = function.blocks().size();
  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock &block) {
                       return reachableBlocks.find(block.name()) ==
                              reachableBlocks.end();
                     }),
      function.blocks().end());
  changed |= oldBlockCount != function.blocks().size();

  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        const auto found = values.find(instruction.result());
        if (found != values.end() &&
            found->second.kind == LatticeKind::Constant &&
            rewriteableConstants.find(instruction.result()) !=
                rewriteableConstants.end() &&
            detail::isSideEffectFree(instruction)) {
          changed = true;
          continue;
        }
      }

      auto operands = rewriteOperands(instruction, values, rewriteableConstants);
      if (instruction.opcode() == "br" && operands.size() == 3) {
        long long condition = 0;
        if (constantValueForOperand(operands[0], values, condition)) {
          const std::string target = condition != 0 ? operands[1] : operands[2];
          rewritten.emplace_back("br", std::vector<std::string>{target});
          changed = true;
          continue;
        }
      }

      if (operands != instruction.operands()) {
        changed = true;
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  std::unordered_map<std::string, std::unordered_set<std::string>>
      predecessors;
  for (const auto &block : function.blocks()) {
    predecessors.emplace(block.name(), std::unordered_set<std::string>{});
  }
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto &operands = terminator->operands();
    if (operands.size() == 1) {
      predecessors[operands[0]].insert(block.name());
    } else if (operands.size() == 3) {
      predecessors[operands[1]].insert(block.name());
      predecessors[operands[2]].insert(block.name());
    }
  }

  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    const auto predIt = predecessors.find(block.name());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }

      std::vector<std::string> operands;
      const auto &oldOperands = instruction.operands();
      if (!oldOperands.empty()) {
        operands.push_back(oldOperands.front());
      }
      std::unordered_set<std::string> seenPreds;
      for (std::size_t i = 1; i + 1 < oldOperands.size(); i += 2) {
        const auto &value = oldOperands[i];
        const auto &pred = oldOperands[i + 1];
        if (predIt == predecessors.end() ||
            predIt->second.find(pred) == predIt->second.end() ||
            !seenPreds.insert(pred).second) {
          continue;
        }
        operands.push_back(value);
        operands.push_back(pred);
      }
      if (operands.size() != oldOperands.size()) {
        changed = true;
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  return changed;
}

bool runSccp(Function &function) {
  std::unordered_map<std::string, LatticeValue> values;
  std::unordered_set<std::string> reachableBlocks;
  std::unordered_set<std::string> executableEdges;
  solveFunction(function, values, reachableBlocks, executableEdges);
  return rewriteFunction(function, values, reachableBlocks);
}

} // namespace

bool SparseConditionalConstantPropagationPass::run(
    compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    runSccp(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
