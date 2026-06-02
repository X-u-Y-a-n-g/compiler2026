#include "../../../include/compiler/midend/pass/CompareSimplificationPass.h"

#include <algorithm>
#include <optional>
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

struct CompareExpr {
  std::string predicate;
  std::string lhs;
  std::string rhs;
};

bool isLocalValue(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

bool isIntegerCompare(const Instruction &instruction) {
  return instruction.opcode().rfind("icmp.", 0) == 0 &&
         instruction.operands().size() == 2;
}

std::optional<std::string> inversePredicate(const std::string &predicate) {
  if (predicate == "eq") {
    return "ne";
  }
  if (predicate == "ne") {
    return "eq";
  }
  if (predicate == "slt") {
    return "sge";
  }
  if (predicate == "sle") {
    return "sgt";
  }
  if (predicate == "sgt") {
    return "sle";
  }
  if (predicate == "sge") {
    return "slt";
  }
  return std::nullopt;
}

std::string swappedPredicate(const std::string &predicate) {
  if (predicate == "slt") {
    return "sgt";
  }
  if (predicate == "sle") {
    return "sge";
  }
  if (predicate == "sgt") {
    return "slt";
  }
  if (predicate == "sge") {
    return "sle";
  }
  return predicate;
}

CompareExpr normalizeCompare(std::string predicate, std::string lhs,
                             std::string rhs) {
  if (rhs < lhs) {
    std::swap(lhs, rhs);
    predicate = swappedPredicate(predicate);
  }
  return CompareExpr{std::move(predicate), std::move(lhs), std::move(rhs)};
}

std::string compareKey(const CompareExpr &expr) {
  return expr.predicate + "\n" + expr.lhs + "\n" + expr.rhs;
}

std::optional<std::string> inverseKey(const CompareExpr &expr) {
  const auto inverse = inversePredicate(expr.predicate);
  if (!inverse.has_value()) {
    return std::nullopt;
  }
  return compareKey(normalizeCompare(*inverse, expr.lhs, expr.rhs));
}

std::unordered_map<std::string, const Instruction *>
definitionMap(const Function &function) {
  std::unordered_map<std::string, const Instruction *> definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions[instruction.result()] = &instruction;
      }
    }
  }
  return definitions;
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
predecessorMap(const Function &function) {
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

std::unordered_set<std::string> edgeFacts(
    const BasicBlock &from, const std::string &to,
    const std::unordered_map<std::string, const Instruction *> &definitions) {
  std::unordered_set<std::string> facts;
  const auto *terminator = from.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3) {
    return facts;
  }
  const auto &operands = terminator->operands();
  if (!isLocalValue(operands[0])) {
    return facts;
  }
  const auto defIt = definitions.find(operands[0]);
  if (defIt == definitions.end() || !isIntegerCompare(*defIt->second)) {
    return facts;
  }
  const auto &cmp = *defIt->second;
  const auto expr = normalizeCompare(cmp.opcode().substr(5), cmp.operands()[0],
                                     cmp.operands()[1]);
  if (to == operands[1]) {
    facts.insert(compareKey(expr));
  }
  if (to == operands[2]) {
    if (const auto inverse = inverseKey(expr); inverse.has_value()) {
      facts.insert(*inverse);
    }
  }
  return facts;
}

std::unordered_map<std::string, std::unordered_set<std::string>>
solveFacts(const Function &function) {
  const auto definitions = definitionMap(function);
  const auto predecessors = predecessorMap(function);
  std::unordered_map<std::string, const BasicBlock *> blocks;
  for (const auto &block : function.blocks()) {
    blocks.emplace(block.name(), &block);
  }

  std::unordered_map<std::string, std::unordered_set<std::string>> inFacts;
  std::unordered_map<std::string, std::unordered_set<std::string>> outFacts;
  for (const auto &block : function.blocks()) {
    inFacts.emplace(block.name(), std::unordered_set<std::string>{});
    outFacts.emplace(block.name(), std::unordered_set<std::string>{});
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &block : function.blocks()) {
      std::unordered_set<std::string> incoming;
      bool initialized = false;
      const auto predIt = predecessors.find(block.name());
      if (predIt != predecessors.end()) {
        for (const auto &predName : predIt->second) {
          const auto predBlockIt = blocks.find(predName);
          if (predBlockIt == blocks.end()) {
            continue;
          }
          auto facts = outFacts[predName];
          auto edge = edgeFacts(*predBlockIt->second, block.name(), definitions);
          facts.insert(edge.begin(), edge.end());
          if (!initialized) {
            incoming = std::move(facts);
            initialized = true;
            continue;
          }
          for (auto it = incoming.begin(); it != incoming.end();) {
            if (facts.find(*it) == facts.end()) {
              it = incoming.erase(it);
            } else {
              ++it;
            }
          }
        }
      }
      if (incoming != inFacts[block.name()]) {
        inFacts[block.name()] = incoming;
        changed = true;
      }
      if (outFacts[block.name()] != inFacts[block.name()]) {
        outFacts[block.name()] = inFacts[block.name()];
        changed = true;
      }
    }
  }
  return inFacts;
}

std::optional<std::string> simplifySameValueCompare(const CompareExpr &expr) {
  if (expr.lhs != expr.rhs) {
    return std::nullopt;
  }
  if (expr.predicate == "eq" || expr.predicate == "sle" ||
      expr.predicate == "sge") {
    return "1";
  }
  if (expr.predicate == "ne" || expr.predicate == "slt" ||
      expr.predicate == "sgt") {
    return "0";
  }
  return std::nullopt;
}

void rewriteFunctionOperands(
    Function &function, const std::unordered_map<std::string, std::string> &aliases) {
  if (aliases.empty()) {
    return;
  }
  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      rewritten.emplace_back(
          instruction.opcode(), detail::rewriteOperands(instruction.operands(), aliases),
          instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void simplifyFunction(Function &function) {
  const auto inFacts = solveFacts(function);
  std::unordered_map<std::string, std::string> aliases;

  for (auto &block : function.blocks()) {
    const auto factIt = inFacts.find(block.name());
    const auto &facts =
        factIt == inFacts.end() ? std::unordered_set<std::string>{} : factIt->second;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      Instruction current{instruction.opcode(), std::move(operands),
                          instruction.result()};
      if (!current.hasResult() || !isIntegerCompare(current)) {
        rewritten.push_back(std::move(current));
        continue;
      }

      const auto expr = normalizeCompare(current.opcode().substr(5),
                                         current.operands()[0],
                                         current.operands()[1]);
      if (auto same = simplifySameValueCompare(expr); same.has_value()) {
        aliases[current.result()] = *same;
        continue;
      }
      if (facts.find(compareKey(expr)) != facts.end()) {
        aliases[current.result()] = "1";
        continue;
      }
      if (const auto inverse = inverseKey(expr);
          inverse.has_value() && facts.find(*inverse) != facts.end()) {
        aliases[current.result()] = "0";
        continue;
      }
      rewritten.push_back(std::move(current));
    }
    block.instructions() = std::move(rewritten);
  }

  rewriteFunctionOperands(function, aliases);
}

} // namespace

bool CompareSimplificationPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    simplifyFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
