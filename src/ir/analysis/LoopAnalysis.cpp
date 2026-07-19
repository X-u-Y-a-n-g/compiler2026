#include "../../../include/compiler/ir/analysis/LoopAnalysis.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
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

bool parseInteger(const std::string& text, long long& value) {
  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(text.c_str(), &end, 0);
  if (errno == ERANGE || end == nullptr || end == text.c_str() ||
      *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

std::optional<std::string> phiIncoming(
    const compiler::ir::core::Instruction& phi,
    const std::string& predecessor) {
  if (phi.opcode() != "phi") return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) {
      return phi.operands()[index];
    }
  }
  return std::nullopt;
}

std::optional<CanonicalInductionVariable> findCanonicalInduction(
    const compiler::ir::core::Function& function, const NaturalLoop& loop) {
  if (!loop.preheader.has_value() || !loop.hasUniqueLatch) return std::nullopt;
  const std::string& preheaderName =
      function.blocks()[*loop.preheader].name();
  const std::string& latchName = function.blocks()[loop.latches.front()].name();
  std::unordered_map<std::string,
                     const compiler::ir::core::Instruction*> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
      }
    }
  }

  for (const auto& phi : function.blocks()[loop.header].instructions()) {
    if (phi.opcode() != "phi" || phi.operands().empty() || !phi.hasResult()) {
      continue;
    }
    const auto initial = phiIncoming(phi, preheaderName);
    const auto updateName = phiIncoming(phi, latchName);
    if (!initial.has_value() || !updateName.has_value()) continue;
    const auto updateFound = definitions.find(*updateName);
    if (updateFound == definitions.end()) continue;
    const auto directStep = [&](const std::string& value)
        -> std::optional<long long> {
      const auto found = definitions.find(value);
      if (found == definitions.end()) return std::nullopt;
      const auto& update = *found->second;
      if ((update.opcode() != "add" && update.opcode() != "sub") ||
          update.operands().size() != 2) {
        return std::nullopt;
      }
      long long step = 0;
      if (update.opcode() == "add" &&
          update.operands()[0] == phi.result() &&
          parseInteger(update.operands()[1], step)) {
        return step;
      }
      if (update.opcode() == "add" &&
          update.operands()[1] == phi.result() &&
          parseInteger(update.operands()[0], step)) {
        return step;
      }
      if (update.opcode() == "sub" &&
          update.operands()[0] == phi.result() &&
          parseInteger(update.operands()[1], step)) {
        return -step;
      }
      return std::nullopt;
    };
    std::optional<long long> inductionStep = directStep(*updateName);
    if (!inductionStep.has_value() &&
        updateFound->second->opcode() == "phi") {
      for (std::size_t index = 1;
           index + 1 < updateFound->second->operands().size(); index += 2) {
        const auto incomingStep =
            directStep(updateFound->second->operands()[index]);
        if (!incomingStep.has_value() ||
            (inductionStep.has_value() &&
             *inductionStep != *incomingStep)) {
          inductionStep.reset();
          break;
        }
        inductionStep = incomingStep;
      }
    }
    if (!inductionStep.has_value() || *inductionStep == 0) continue;

    CanonicalInductionVariable induction{phi.result(), phi.operands()[0],
                                         *initial, *updateName,
                                         *inductionStep, {}, {}};
    const auto* terminator = function.blocks()[loop.header].terminator();
    if (terminator != nullptr && terminator->opcode() == "br" &&
        terminator->operands().size() == 3) {
      const auto comparison = definitions.find(terminator->operands()[0]);
      if (comparison != definitions.end() &&
          comparison->second->opcode().rfind("icmp.", 0) == 0 &&
          comparison->second->operands().size() == 2) {
        const auto& operands = comparison->second->operands();
        if (operands[0] == phi.result()) {
          induction.predicate = comparison->second->opcode().substr(5);
          induction.bound = operands[1];
        } else if (operands[1] == phi.result()) {
          induction.predicate = comparison->second->opcode().substr(5);
          induction.bound = operands[0];
        }
      }
    }
    return induction;
  }
  return std::nullopt;
}

bool isLoopClosedSSA(const compiler::ir::core::Function& function,
                     const NaturalLoop& loop) {
  std::unordered_set<std::string> definitions;
  std::unordered_set<std::size_t> exitTargets;
  for (const auto block : loop.blocks) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult()) definitions.insert(instruction.result());
    }
  }
  for (const auto& edge : loop.exitEdges) exitTargets.insert(edge.second);
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (loop.contains(block)) continue;
    for (const auto& instruction : function.blocks()[block].instructions()) {
      for (std::size_t index = 0; index < instruction.operands().size(); ++index) {
        if (definitions.find(instruction.operands()[index]) == definitions.end()) {
          continue;
        }
        const bool lcssaPhi = instruction.opcode() == "phi" &&
                              index % 2 == 1 && index + 1 < instruction.operands().size() &&
                              exitTargets.find(block) != exitTargets.end();
        if (!lcssaPhi) return false;
      }
    }
  }
  return true;
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

  std::vector<bool> reachable(count, false);
  std::deque<std::size_t> reachableWorklist{0};
  reachable[0] = true;
  while (!reachableWorklist.empty()) {
    const auto block = reachableWorklist.front();
    reachableWorklist.pop_front();
    for (const auto successor : successors_[block]) {
      if (!reachable[successor]) {
        reachable[successor] = true;
        reachableWorklist.push_back(successor);
      }
    }
  }

  dominators_.assign(count, std::vector<bool>(count, false));
  for (std::size_t block = 0; block < count; ++block) {
    if (!reachable[block]) {
      dominators_[block][block] = true;
      continue;
    }
    for (std::size_t candidate = 0; candidate < count; ++candidate) {
      dominators_[block][candidate] = reachable[candidate];
    }
  }
  dominators_[0].assign(count, false);
  dominators_[0][0] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (!reachable[block]) continue;
      std::vector<bool> next(count, true);
      for (std::size_t candidate = 0; candidate < count; ++candidate) {
        next[candidate] = reachable[candidate];
      }
      bool sawReachablePredecessor = false;
      for (const auto predecessor : predecessors_[block]) {
        if (!reachable[predecessor]) continue;
        sawReachablePredecessor = true;
        for (std::size_t candidate = 0; candidate < count; ++candidate) {
          next[candidate] = next[candidate] && dominators_[predecessor][candidate];
        }
      }
      if (!sawReachablePredecessor) next.assign(count, false);
      next[block] = true;
      if (next != dominators_[block]) {
        dominators_[block] = std::move(next);
        changed = true;
      }
    }
  }

  std::unordered_map<std::size_t, std::size_t> loopForHeader;
  for (std::size_t latch = 0; latch < count; ++latch) {
    if (!reachable[latch]) continue;
    for (const auto header : successors_[latch]) {
      if (!reachable[header] || !dominates(header, latch)) continue;
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
    loop.induction = findCanonicalInduction(function, loop);
    loop.isLoopClosedSSA = isLoopClosedSSA(function, loop);
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
