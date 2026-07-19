#include "../../../include/compiler/ir/analysis/LoopAnalysis.h"

#include <algorithm>
#include <deque>
#include <unordered_map>

#include "../../../include/compiler/ir/core/Function.h"

namespace compiler::ir::analysis {
namespace {

std::vector<std::string> branchTargets(
    const compiler::ir::core::BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") return {};
  const auto& operands = terminator->operands();
  if (operands.size() == 1) return {operands[0]};
  if (operands.size() == 3) return {operands[1], operands[2]};
  return {};
}

}  // namespace

LoopAnalysis::LoopAnalysis(const compiler::ir::core::Function& function) {
  const std::size_t count = function.blocks().size();
  predecessors_.resize(count);
  successors_.resize(count);
  if (count == 0) return;

  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t index = 0; index < count; ++index) {
    indices.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < count; ++index) {
    for (const auto& targetName : branchTargets(function.blocks()[index])) {
      const auto target = indices.find(targetName);
      if (target == indices.end()) continue;
      successors_[index].push_back(target->second);
      predecessors_[target->second].push_back(index);
    }
  }

  dominators_.assign(count, std::vector<bool>(count, true));
  dominators_[0].assign(count, false);
  dominators_[0][0] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      std::vector<bool> next(count, true);
      if (predecessors_[block].empty()) next.assign(count, false);
      for (const auto predecessor : predecessors_[block]) {
        for (std::size_t candidate = 0; candidate < count; ++candidate) {
          next[candidate] = next[candidate] && dominators_[predecessor][candidate];
        }
      }
      next[block] = true;
      if (next != dominators_[block]) {
        dominators_[block] = std::move(next);
        changed = true;
      }
    }
  }

  std::unordered_map<std::size_t, std::size_t> loopForHeader;
  for (std::size_t latch = 0; latch < count; ++latch) {
    for (const auto header : successors_[latch]) {
      if (!dominates(header, latch)) continue;
      std::size_t loopIndex;
      const auto existing = loopForHeader.find(header);
      if (existing == loopForHeader.end()) {
        loopIndex = loops_.size();
        loopForHeader.emplace(header, loopIndex);
        NaturalLoop loop;
        loop.header = header;
        loop.blocks.insert(header);
        loops_.push_back(std::move(loop));
      } else {
        loopIndex = existing->second;
      }
      auto& loop = loops_[loopIndex];
      loop.latches.push_back(latch);
      std::deque<std::size_t> worklist{latch};
      while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop_front();
        if (!loop.blocks.insert(block).second || block == header) continue;
        for (const auto predecessor : predecessors_[block]) {
          worklist.push_back(predecessor);
        }
      }
    }
  }

  for (auto& loop : loops_) {
    std::sort(loop.latches.begin(), loop.latches.end());
    loop.latches.erase(std::unique(loop.latches.begin(), loop.latches.end()),
                       loop.latches.end());
    loop.hasUniqueLatch = loop.latches.size() == 1;
    for (const auto predecessor : predecessors_[loop.header]) {
      if (!loop.contains(predecessor)) loop.outsidePredecessors.push_back(predecessor);
    }
    if (loop.outsidePredecessors.size() == 1) {
      const auto candidate = loop.outsidePredecessors.front();
      if (successors_[candidate].size() == 1 &&
          successors_[candidate].front() == loop.header) {
        loop.preheader = candidate;
      }
    }
    std::unordered_set<std::size_t> exitTargets;
    for (const auto block : loop.blocks) {
      for (const auto successor : successors_[block]) {
        if (!loop.contains(successor)) {
          loop.exitEdges.emplace_back(block, successor);
          exitTargets.insert(successor);
        }
      }
    }
    loop.hasDedicatedExits = true;
    for (const auto target : exitTargets) {
      for (const auto predecessor : predecessors_[target]) {
        if (!loop.contains(predecessor)) loop.hasDedicatedExits = false;
      }
    }
  }

  std::sort(loops_.begin(), loops_.end(), [](const NaturalLoop& lhs,
                                             const NaturalLoop& rhs) {
    if (lhs.blocks.size() != rhs.blocks.size()) return lhs.blocks.size() < rhs.blocks.size();
    return lhs.header < rhs.header;
  });
}

unsigned LoopAnalysis::loopDepth(std::size_t block) const {
  unsigned depth = 0;
  for (const auto& loop : loops_) depth += static_cast<unsigned>(loop.contains(block));
  return depth;
}

bool LoopAnalysis::dominates(std::size_t dominator, std::size_t block) const {
  return block < dominators_.size() && dominator < dominators_[block].size() &&
         dominators_[block][dominator];
}

}  // namespace compiler::ir::analysis
