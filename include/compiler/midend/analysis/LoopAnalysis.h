#pragma once

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../ir/core/Function.h"

namespace compiler::midend::analysis {

struct LoopBlockInfo {
  std::string name;
  std::vector<std::size_t> preds;
  std::vector<std::size_t> succs;
};

struct NaturalLoop {
  std::size_t header = 0;
  std::size_t latch = 0;
  std::optional<std::size_t> preheader;
  std::unordered_set<std::size_t> blocks;
};

inline std::vector<LoopBlockInfo> buildLoopCFG(const compiler::ir::core::Function& function) {
  std::vector<LoopBlockInfo> cfg;
  cfg.reserve(function.blocks().size());
  std::unordered_map<std::string, std::size_t> indexByName;

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    cfg.push_back(LoopBlockInfo{function.blocks()[i].name(), {}, {}});
    indexByName.emplace(function.blocks()[i].name(), i);
  }

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    const auto* terminator = function.blocks()[i].terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto& operands = terminator->operands();
    if (operands.size() == 1) {
      if (const auto target = indexByName.find(operands[0]); target != indexByName.end()) {
        cfg[i].succs.push_back(target->second);
      }
    } else if (operands.size() == 3) {
      const auto trueTarget = indexByName.find(operands[1]);
      const auto falseTarget = indexByName.find(operands[2]);
      if (trueTarget != indexByName.end()) {
        cfg[i].succs.push_back(trueTarget->second);
      }
      if (falseTarget != indexByName.end() &&
          (trueTarget == indexByName.end() || falseTarget->second != trueTarget->second)) {
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

inline std::vector<bool> computeLoopReachable(const std::vector<LoopBlockInfo>& cfg) {
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
      if (!reachable[succ]) {
        reachable[succ] = true;
        worklist.push_back(succ);
      }
    }
  }
  return reachable;
}

inline std::vector<std::size_t> computeLoopReversePostOrder(
    const std::vector<LoopBlockInfo>& cfg, const std::vector<bool>& reachable) {
  std::vector<std::size_t> order;
  if (cfg.empty()) {
    return order;
  }
  std::vector<bool> visited(cfg.size(), false);
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  visited[0] = true;
  stack.emplace_back(0, 0);
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

inline std::size_t intersectLoopIdoms(std::size_t lhs, std::size_t rhs,
                                      const std::vector<int>& idom,
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

inline std::vector<int> computeLoopImmediateDominators(
    const std::vector<LoopBlockInfo>& cfg, const std::vector<bool>& reachable) {
  std::vector<int> idom(cfg.size(), -1);
  if (cfg.empty()) {
    return idom;
  }
  const auto rpo = computeLoopReversePostOrder(cfg, reachable);
  std::vector<std::size_t> rpoIndex(cfg.size(), cfg.size());
  for (std::size_t i = 0; i < rpo.size(); ++i) {
    rpoIndex[rpo[i]] = i;
  }

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
        next = next.has_value() ? intersectLoopIdoms(pred, *next, idom, rpoIndex) : pred;
      }
      if (next.has_value() && idom[block] != static_cast<int>(*next)) {
        idom[block] = static_cast<int>(*next);
        changed = true;
      }
    }
  }
  return idom;
}

inline bool loopDominates(std::size_t dominator, std::size_t block,
                          const std::vector<int>& idom) {
  if (dominator == block) {
    return true;
  }
  while (block < idom.size() && idom[block] >= 0 && static_cast<std::size_t>(idom[block]) != block) {
    block = static_cast<std::size_t>(idom[block]);
    if (block == dominator) {
      return true;
    }
  }
  return false;
}

inline NaturalLoop buildNaturalLoop(const std::vector<LoopBlockInfo>& cfg, std::size_t header,
                                    std::size_t latch) {
  NaturalLoop loop;
  loop.header = header;
  loop.latch = latch;
  loop.blocks.insert(header);
  loop.blocks.insert(latch);

  std::deque<std::size_t> worklist;
  worklist.push_back(latch);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto pred : cfg[block].preds) {
      if (loop.blocks.insert(pred).second) {
        worklist.push_back(pred);
      }
    }
  }

  for (const auto pred : cfg[header].preds) {
    if (loop.blocks.find(pred) == loop.blocks.end()) {
      if (loop.preheader.has_value()) {
        loop.preheader = std::nullopt;
        return loop;
      }
      loop.preheader = pred;
    }
  }
  return loop;
}

inline std::vector<NaturalLoop> analyzeNaturalLoops(const compiler::ir::core::Function& function) {
  const auto cfg = buildLoopCFG(function);
  const auto reachable = computeLoopReachable(cfg);
  const auto idom = computeLoopImmediateDominators(cfg, reachable);
  std::vector<NaturalLoop> loops;
  for (std::size_t latch = 0; latch < cfg.size(); ++latch) {
    if (!reachable[latch]) {
      continue;
    }
    for (const auto header : cfg[latch].succs) {
      if (reachable[header] && loopDominates(header, latch, idom)) {
        loops.push_back(buildNaturalLoop(cfg, header, latch));
      }
    }
  }
  std::sort(loops.begin(), loops.end(), [](const NaturalLoop& lhs, const NaturalLoop& rhs) {
    return lhs.blocks.size() < rhs.blocks.size();
  });
  return loops;
}

}  // namespace compiler::midend::analysis
