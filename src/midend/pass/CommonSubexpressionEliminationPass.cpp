#include "../../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::midend::analysis::FunctionEffectAnalysis;

std::string calledFunctionName(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& callee = instruction.operands().front();
  if (callee.size() < 2 || callee.front() != '@') {
    return {};
  }
  return callee.substr(1);
}

struct BlockInfo {
  std::string name;
  std::vector<std::size_t> preds;
  std::vector<std::size_t> succs;
};

std::vector<BlockInfo> buildCFG(const Function& function) {
  std::vector<BlockInfo> cfg;
  cfg.reserve(function.blocks().size());
  std::unordered_map<std::string, std::size_t> indexByName;

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    cfg.push_back(BlockInfo{function.blocks()[i].name(), {}, {}});
    indexByName.emplace(function.blocks()[i].name(), i);
  }

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    const auto* terminator = function.blocks()[i].terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }

    const auto& operands = terminator->operands();
    if (operands.size() == 1) {
      const auto target = indexByName.find(operands[0]);
      if (target != indexByName.end()) {
        cfg[i].succs.push_back(target->second);
      }
    } else if (operands.size() == 3) {
      const auto trueTarget = indexByName.find(operands[1]);
      const auto falseTarget = indexByName.find(operands[2]);
      if (trueTarget != indexByName.end()) {
        cfg[i].succs.push_back(trueTarget->second);
      }
      if (falseTarget != indexByName.end()) {
        cfg[i].succs.push_back(falseTarget->second);
      }
    }
  }

  for (std::size_t i = 0; i < cfg.size(); ++i) {
    for (const auto succ : cfg[i].succs) {
      cfg[succ].preds.push_back(i);
    }
  }

  return cfg;
}

std::vector<bool> computeReachable(const std::vector<BlockInfo>& cfg) {
  std::vector<bool> reachable(cfg.size(), false);
  if (cfg.empty()) {
    return reachable;
  }

  std::deque<std::size_t> worklist;
  reachable[0] = true;
  worklist.push_back(0);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto succ : cfg[block].succs) {
      if (reachable[succ]) {
        continue;
      }
      reachable[succ] = true;
      worklist.push_back(succ);
    }
  }
  return reachable;
}

std::vector<std::size_t> computeReversePostOrder(const std::vector<BlockInfo>& cfg,
                                                 const std::vector<bool>& reachable) {
  std::vector<std::size_t> order;
  if (cfg.empty()) {
    return order;
  }

  std::vector<bool> visited(cfg.size(), false);
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  stack.emplace_back(0, 0);
  visited[0] = true;
  while (!stack.empty()) {
    auto& [block, nextSucc] = stack.back();
    if (nextSucc < cfg[block].succs.size()) {
      const auto succ = cfg[block].succs[nextSucc++];
      if (reachable[succ] && !visited[succ]) {
        visited[succ] = true;
        stack.emplace_back(succ, 0);
      }
      continue;
    }
    order.push_back(block);
    stack.pop_back();
  }
  std::reverse(order.begin(), order.end());
  return order;
}

std::size_t intersectIdoms(std::size_t lhs, std::size_t rhs, const std::vector<int>& idom,
                           const std::vector<std::size_t>& rpoIndex) {
  while (lhs != rhs) {
    while (rpoIndex[lhs] > rpoIndex[rhs]) {
      lhs = static_cast<std::size_t>(idom[lhs]);
    }
    while (rpoIndex[rhs] > rpoIndex[lhs]) {
      rhs = static_cast<std::size_t>(idom[rhs]);
    }
  }
  return lhs;
}

std::vector<std::vector<std::size_t>> buildDominatorTree(const std::vector<BlockInfo>& cfg) {
  const std::size_t n = cfg.size();
  std::vector<std::vector<std::size_t>> tree(n);
  if (n == 0) {
    return tree;
  }

  const auto reachable = computeReachable(cfg);
  const auto rpo = computeReversePostOrder(cfg, reachable);
  std::vector<std::size_t> rpoIndex(n, n);
  for (std::size_t i = 0; i < rpo.size(); ++i) {
    rpoIndex[rpo[i]] = i;
  }

  std::vector<int> idom(n, -1);
  idom[0] = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto block : rpo) {
      if (block == 0) {
        continue;
      }

      std::optional<std::size_t> next;
      for (const auto pred : cfg[block].preds) {
        if (!reachable[pred] || idom[pred] < 0) {
          continue;
        }
        if (!next.has_value()) {
          next = pred;
        } else {
          next = intersectIdoms(pred, *next, idom, rpoIndex);
        }
      }
      if (next.has_value() && idom[block] != static_cast<int>(*next)) {
        idom[block] = static_cast<int>(*next);
        changed = true;
      }
    }
  }

  for (std::size_t block = 1; block < n; ++block) {
    if (reachable[block] && idom[block] >= 0) {
      tree[static_cast<std::size_t>(idom[block])].push_back(block);
    }
  }

  return tree;
}

bool isCommutative(const std::string& opcode) {
  return opcode == "add" || opcode == "mul" || opcode == "and" || opcode == "or" ||
         opcode == "xor" ||
         opcode == "fadd" || opcode == "fmul" || opcode == "icmp.eq" || opcode == "icmp.ne" ||
         opcode == "fcmp.oeq" || opcode == "fcmp.one";
}

bool isCseCandidate(const Instruction& instruction,
                    const FunctionEffectAnalysis& functionEffects) {
  if (!instruction.hasResult()) {
    return false;
  }

  if (instruction.opcode() == "call") {
    const auto callee = calledFunctionName(instruction);
    return !callee.empty() && functionEffects.isReadNone(callee);
  }
  if (!detail::isSideEffectFree(instruction)) {
    return false;
  }

  const auto& opcode = instruction.opcode();
  return opcode != "alloca" && opcode != "load" && opcode != "phi";
}

std::string expressionKey(const Instruction& instruction) {
  std::string key = instruction.opcode();
  auto operands = instruction.operands();
  if (isCommutative(instruction.opcode()) && operands.size() == 2) {
    std::sort(operands.begin(), operands.end());
  }

  for (const auto& operand : operands) {
    key.push_back('\n');
    key.append(operand);
  }
  return key;
}

void rewriteFunctionOperands(
    Function& function, const std::unordered_map<std::string, std::string>& replacements) {
  if (replacements.empty()) {
    return;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.push_back(Instruction{
          instruction.opcode(), detail::rewriteOperands(instruction.operands(), replacements),
          instruction.result()});
    }
    block.instructions() = std::move(rewritten);
  }
}

void runCse(Function& function,
            const FunctionEffectAnalysis& functionEffects) {
  const auto cfg = buildCFG(function);
  if (cfg.empty()) {
    return;
  }

  const auto domTree = buildDominatorTree(cfg);
  std::unordered_map<std::string, std::string> scopedAliases;
  std::unordered_map<std::string, std::string> allAliases;
  std::unordered_map<std::string, std::string> availableExpressions;

  const std::function<void(std::size_t)> visit = [&](std::size_t index) {
    auto& block = function.blocks()[index];
    std::vector<std::string> insertedExpressions;
    std::vector<std::string> insertedAliases;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), scopedAliases);
      Instruction rewrittenInstruction{instruction.opcode(), std::move(operands),
                                       instruction.result()};

      if (isCseCandidate(rewrittenInstruction, functionEffects)) {
        const auto key = expressionKey(rewrittenInstruction);
        const auto available = availableExpressions.find(key);
        if (available != availableExpressions.end()) {
          const auto replacement = detail::resolveAlias(available->second, scopedAliases);
          scopedAliases[rewrittenInstruction.result()] = replacement;
          allAliases[rewrittenInstruction.result()] = replacement;
          insertedAliases.push_back(rewrittenInstruction.result());
          continue;
        }

        availableExpressions.emplace(key, rewrittenInstruction.result());
        insertedExpressions.push_back(key);
      }

      rewritten.push_back(std::move(rewrittenInstruction));
    }

    block.instructions() = std::move(rewritten);

    for (const auto child : domTree[index]) {
      visit(child);
    }

    for (auto it = insertedAliases.rbegin(); it != insertedAliases.rend(); ++it) {
      scopedAliases.erase(*it);
    }
    for (auto it = insertedExpressions.rbegin(); it != insertedExpressions.rend(); ++it) {
      availableExpressions.erase(*it);
    }
  };

  visit(0);
  rewriteFunctionOperands(function, allAliases);
}

}  // namespace

void CommonSubexpressionEliminationPass::run(compiler::ir::core::Module& module) {
  const FunctionEffectAnalysis functionEffects(module);
  for (auto& function : module.functions()) {
    runCse(function, functionEffects);
  }
}

}  // namespace compiler::midend::pass
