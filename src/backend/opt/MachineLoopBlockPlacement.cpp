#include "../../../include/compiler/backend/opt/MachineLoopBlockPlacement.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::common::CFGBuilder;
using compiler::backend::common::ControlFlowGraph;
using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;

constexpr std::size_t kMaxAnalyzedInstructions = 4096;

struct Loop {
  std::size_t header = 0;
  std::unordered_set<std::size_t> blocks;
  std::vector<std::size_t> latches;
};

std::vector<bool> reachableBlocks(const ControlFlowGraph& cfg) {
  std::vector<bool> reachable(cfg.blocks.size(), false);
  if (cfg.blocks.empty()) {
    return reachable;
  }
  std::deque<std::size_t> worklist{0};
  reachable[0] = true;
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto successor : cfg.successors[block]) {
      if (!reachable[successor]) {
        reachable[successor] = true;
        worklist.push_back(successor);
      }
    }
  }
  return reachable;
}

std::vector<std::unordered_set<std::size_t>> dominators(
    const ControlFlowGraph& cfg, const std::vector<bool>& reachable) {
  std::unordered_set<std::size_t> allReachable;
  for (std::size_t block = 0; block < reachable.size(); ++block) {
    if (reachable[block]) {
      allReachable.insert(block);
    }
  }

  std::vector<std::unordered_set<std::size_t>> result(cfg.blocks.size());
  if (cfg.blocks.empty()) {
    return result;
  }
  result[0].insert(0);
  for (std::size_t block = 1; block < cfg.blocks.size(); ++block) {
    if (reachable[block]) {
      result[block] = allReachable;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < cfg.blocks.size(); ++block) {
      if (!reachable[block]) {
        continue;
      }
      std::optional<std::unordered_set<std::size_t>> intersection;
      for (const auto predecessor : cfg.predecessors[block]) {
        if (!reachable[predecessor]) {
          continue;
        }
        if (!intersection.has_value()) {
          intersection = result[predecessor];
          continue;
        }
        std::unordered_set<std::size_t> next;
        for (const auto candidate : *intersection) {
          if (result[predecessor].find(candidate) !=
              result[predecessor].end()) {
            next.insert(candidate);
          }
        }
        intersection = std::move(next);
      }
      auto next = intersection.value_or(std::unordered_set<std::size_t>{});
      next.insert(block);
      if (next != result[block]) {
        result[block] = std::move(next);
        changed = true;
      }
    }
  }
  return result;
}

std::vector<Loop> naturalLoops(
    const ControlFlowGraph& cfg, const std::vector<bool>& reachable,
    const std::vector<std::unordered_set<std::size_t>>& dom) {
  std::unordered_map<std::size_t, Loop> byHeader;
  for (std::size_t latch = 0; latch < cfg.blocks.size(); ++latch) {
    if (!reachable[latch]) {
      continue;
    }
    for (const auto header : cfg.successors[latch]) {
      if (dom[latch].find(header) == dom[latch].end()) {
        continue;
      }
      std::unordered_set<std::size_t> blocks{header, latch};
      std::deque<std::size_t> worklist;
      if (latch != header) {
        worklist.push_back(latch);
      }
      while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop_front();
        for (const auto predecessor : cfg.predecessors[block]) {
          if (blocks.insert(predecessor).second && predecessor != header) {
            worklist.push_back(predecessor);
          }
        }
      }
      auto& loop = byHeader[header];
      loop.header = header;
      loop.blocks.insert(blocks.begin(), blocks.end());
      if (std::find(loop.latches.begin(), loop.latches.end(), latch) ==
          loop.latches.end()) {
        loop.latches.push_back(latch);
      }
    }
  }

  std::vector<Loop> loops;
  loops.reserve(byHeader.size());
  for (auto& [unused, loop] : byHeader) {
    loops.push_back(std::move(loop));
  }
  std::sort(loops.begin(), loops.end(), [](const Loop& lhs, const Loop& rhs) {
    return lhs.blocks.size() < rhs.blocks.size();
  });
  return loops;
}

bool allBlocksHaveExplicitTerminators(
    const MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  return std::all_of(function.blocks.begin(), function.blocks.end(),
                     [&](const MachineBlock& block) {
                       if (block.instructions.empty()) {
                         return false;
                       }
                       const auto& terminator = block.instructions.back();
                       return adapter.isConditionalBranch(terminator) ||
                              adapter.isUnconditionalBranch(terminator) ||
                              adapter.isReturn(terminator);
                     });
}

std::optional<std::pair<std::string, std::string>> loopBodyAndExit(
    const Loop& loop, const ControlFlowGraph& cfg,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  const auto& header = *cfg.blocks[loop.header];
  if (header.instructions.empty() ||
      !adapter.isConditionalBranch(header.instructions.back())) {
    return std::nullopt;
  }
  const auto targets = adapter.branchTargets(header.instructions.back());
  if (targets.size() != 2) {
    return std::nullopt;
  }
  const auto first = cfg.indexByName.find(targets[0]);
  const auto second = cfg.indexByName.find(targets[1]);
  if (first == cfg.indexByName.end() || second == cfg.indexByName.end()) {
    return std::nullopt;
  }
  const bool firstInside = loop.blocks.find(first->second) != loop.blocks.end();
  const bool secondInside = loop.blocks.find(second->second) != loop.blocks.end();
  if (firstInside == secondInside) {
    return std::nullopt;
  }
  return firstInside ? std::pair{targets[0], targets[1]}
                     : std::pair{targets[1], targets[0]};
}

std::optional<std::string> canonicalLatch(
    const Loop& loop, const ControlFlowGraph& cfg,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (loop.latches.size() != 1 || loop.latches.front() == loop.header) {
    return std::nullopt;
  }
  const auto latchIndex = loop.latches.front();
  const auto& latch = *cfg.blocks[latchIndex];
  if (latch.instructions.empty() ||
      !adapter.isUnconditionalBranch(latch.instructions.back())) {
    return std::nullopt;
  }
  const auto targets = adapter.branchTargets(latch.instructions.back());
  if (targets.size() != 1 || targets.front() != cfg.blocks[loop.header]->name) {
    return std::nullopt;
  }
  return latch.name;
}

bool hasUniqueExternalEntry(const Loop& loop, const ControlFlowGraph& cfg) {
  std::size_t externalPredecessors = 0;
  for (const auto predecessor : cfg.predecessors[loop.header]) {
    if (loop.blocks.find(predecessor) == loop.blocks.end()) {
      ++externalPredecessors;
    }
  }
  return externalPredecessors == 1;
}

bool moveAfter(MachineFunction& function, const std::string& moving,
               const std::string& anchor) {
  if (moving == anchor) {
    return false;
  }
  std::optional<std::size_t> movingIndex;
  std::optional<std::size_t> anchorIndex;
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    if (function.blocks[index].name == moving) {
      movingIndex = index;
    }
    if (function.blocks[index].name == anchor) {
      anchorIndex = index;
    }
  }
  if (!movingIndex.has_value() || !anchorIndex.has_value()) {
    return false;
  }
  MachineBlock block = std::move(function.blocks[*movingIndex]);
  function.blocks.erase(function.blocks.begin() +
                        static_cast<std::ptrdiff_t>(*movingIndex));
  if (*movingIndex < *anchorIndex) {
    --*anchorIndex;
  }
  function.blocks.insert(
      function.blocks.begin() + static_cast<std::ptrdiff_t>(*anchorIndex + 1),
      std::move(block));
  return true;
}

std::unordered_map<std::string, std::string> noOpRedirects(
    const MachineFunction& function) {
  std::unordered_map<std::string, std::string> redirects;
  for (std::size_t blockIndex = 1; blockIndex < function.blocks.size();
       ++blockIndex) {
    const auto& block = function.blocks[blockIndex];
    if (block.instructions.empty()) {
      continue;
    }
    bool onlyNoOpCopies = true;
    for (std::size_t index = 0; index + 1 < block.instructions.size(); ++index) {
      const auto& instruction = block.instructions[index];
      if (instruction.opcode != "copy" || !instruction.hasResult ||
          instruction.operands.size() != 1 ||
          instruction.result.kind !=
              compiler::backend::mir::MachineOperandKind::PhysicalRegister ||
          instruction.operands.front().kind !=
              compiler::backend::mir::MachineOperandKind::PhysicalRegister ||
          instruction.result.text != instruction.operands.front().text) {
        onlyNoOpCopies = false;
        break;
      }
    }
    const auto& terminator = block.instructions.back();
    if (onlyNoOpCopies && terminator.opcode == "br" &&
        terminator.operands.size() == 1 &&
        terminator.operands.front().kind ==
            compiler::backend::mir::MachineOperandKind::Label &&
        terminator.operands.front().text != block.name) {
      redirects.emplace(block.name, terminator.operands.front().text);
    }
  }
  return redirects;
}

std::string resolveRedirect(
    std::string name,
    const std::unordered_map<std::string, std::string>& redirects) {
  std::unordered_set<std::string> seen;
  while (seen.insert(name).second) {
    const auto found = redirects.find(name);
    if (found == redirects.end()) {
      break;
    }
    name = found->second;
  }
  return name;
}

std::size_t emittedControlTransfers(
    const MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  const auto redirects = noOpRedirects(function);
  std::size_t transfers = 0;
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    const auto& block = function.blocks[index];
    if (resolveRedirect(block.name, redirects) != block.name ||
        block.instructions.empty()) {
      continue;
    }
    std::optional<std::string> fallthrough;
    for (std::size_t next = index + 1; next < function.blocks.size(); ++next) {
      const auto resolved =
          resolveRedirect(function.blocks[next].name, redirects);
      if (resolved == function.blocks[next].name) {
        fallthrough = resolved;
        break;
      }
    }
    const auto& terminator = block.instructions.back();
    if (adapter.isUnconditionalBranch(terminator)) {
      const auto targets = adapter.branchTargets(terminator);
      if (targets.size() == 1 &&
          (!fallthrough.has_value() ||
           resolveRedirect(targets.front(), redirects) != *fallthrough)) {
        ++transfers;
      }
    } else if (adapter.isConditionalBranch(terminator)) {
      const auto targets = adapter.branchTargets(terminator);
      if (targets.size() != 2) {
        continue;
      }
      ++transfers;
      if (!fallthrough.has_value() ||
          (resolveRedirect(targets[0], redirects) != *fallthrough &&
           resolveRedirect(targets[1], redirects) != *fallthrough)) {
        ++transfers;
      }
    }
  }
  return transfers;
}

bool placeOneLoop(
    MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter,
    std::unordered_set<std::string>& placedHeaders) {
  const auto cfg = CFGBuilder{}.build(function, adapter);
  const auto reachable = reachableBlocks(cfg);
  const auto dom = dominators(cfg, reachable);
  for (const auto& loop : naturalLoops(cfg, reachable, dom)) {
    // Copy the name: moving the header invalidates references into the block
    // vector before the exit is placed relative to that header.
    const std::string headerName = cfg.blocks[loop.header]->name;
    if (loop.header == 0 || placedHeaders.find(headerName) != placedHeaders.end() ||
        !hasUniqueExternalEntry(loop, cfg)) {
      continue;
    }
    const auto bodyAndExit = loopBodyAndExit(loop, cfg, adapter);
    const auto latch = canonicalLatch(loop, cfg, adapter);
    if (!bodyAndExit.has_value() || !latch.has_value() ||
        bodyAndExit->first == headerName || bodyAndExit->second == headerName) {
      continue;
    }

    // First make the hot backedge a physical fallthrough, then make the cold
    // exit the fallthrough of the condition. As every block has an explicit
    // terminator, moving the blocks cannot create or remove CFG edges.
    if (!moveAfter(function, headerName, *latch) ||
        !moveAfter(function, bodyAndExit->second, headerName)) {
      return false;
    }
    placedHeaders.insert(headerName);
    return true;
  }
  return false;
}

}  // namespace

void MachineLoopBlockPlacement::run(
    compiler::backend::mir::MachineModule& module) const {
  const auto& adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  for (auto& function : module.functions) {
    std::size_t instructionCount = 0;
    for (const auto& block : function.blocks) {
      instructionCount += block.instructions.size();
    }
    if (instructionCount > kMaxAnalyzedInstructions) {
      continue;
    }
    if (!allBlocksHaveExplicitTerminators(function, adapter)) {
      continue;
    }
    const auto baselineTransfers = emittedControlTransfers(function, adapter);
    auto originalBlocks = function.blocks;
    std::unordered_set<std::string> placedHeaders;
    while (placeOneLoop(function, adapter, placedHeaders)) {
    }
    // A physical layout change is profitable only if the final emitted code,
    // after the printer's empty-copy redirections, contains fewer control
    // transfers. This also prevents one nested-loop placement from undoing the
    // fallthrough created for another loop.
    if (placedHeaders.empty() ||
        emittedControlTransfers(function, adapter) >= baselineTransfers) {
      function.blocks = std::move(originalBlocks);
    }
  }
}

}  // namespace compiler::backend::opt
