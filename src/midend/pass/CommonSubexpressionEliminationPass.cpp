#include "../../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"

#include <algorithm>
#include <functional>
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

std::unordered_set<std::size_t> intersect(const std::unordered_set<std::size_t>& lhs,
                                          const std::unordered_set<std::size_t>& rhs) {
  std::unordered_set<std::size_t> out;
  const auto& smaller = lhs.size() < rhs.size() ? lhs : rhs;
  const auto& larger = lhs.size() < rhs.size() ? rhs : lhs;
  for (const auto value : smaller) {
    if (larger.find(value) != larger.end()) {
      out.insert(value);
    }
  }
  return out;
}

std::vector<std::vector<std::size_t>> buildDominatorTree(const std::vector<BlockInfo>& cfg) {
  const std::size_t n = cfg.size();
  std::vector<std::vector<std::size_t>> tree(n);
  if (n == 0) {
    return tree;
  }

  std::vector<std::unordered_set<std::size_t>> doms(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (i == 0) {
      doms[i].insert(0);
    } else {
      for (std::size_t j = 0; j < n; ++j) {
        doms[i].insert(j);
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < n; ++block) {
      if (cfg[block].preds.empty()) {
        continue;
      }

      auto next = doms[cfg[block].preds.front()];
      for (std::size_t i = 1; i < cfg[block].preds.size(); ++i) {
        next = intersect(next, doms[cfg[block].preds[i]]);
      }
      next.insert(block);
      if (next != doms[block]) {
        doms[block] = std::move(next);
        changed = true;
      }
    }
  }

  for (std::size_t block = 1; block < n; ++block) {
    std::size_t idom = 0;
    bool foundIdom = false;
    for (const auto candidate : doms[block]) {
      if (candidate == block) {
        continue;
      }

      bool dominatedByAnotherCandidate = false;
      for (const auto other : doms[block]) {
        if (other == block || other == candidate) {
          continue;
        }
        if (doms[candidate].find(other) != doms[candidate].end()) {
          dominatedByAnotherCandidate = true;
          break;
        }
      }

      if (!dominatedByAnotherCandidate) {
        idom = candidate;
        foundIdom = true;
        break;
      }
    }
    tree[foundIdom ? idom : 0].push_back(block);
  }

  return tree;
}

bool isCommutative(const std::string& opcode) {
  return opcode == "add" || opcode == "mul" || opcode == "and" || opcode == "or" ||
         opcode == "fadd" || opcode == "fmul" || opcode == "icmp.eq" || opcode == "icmp.ne" ||
         opcode == "fcmp.oeq" || opcode == "fcmp.one";
}

bool isCseCandidate(const Instruction& instruction) {
  if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
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

void runCse(Function& function) {
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

      if (isCseCandidate(rewrittenInstruction)) {
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
  for (auto& function : module.functions()) {
    runCse(function);
  }
}

}  // namespace compiler::midend::pass
