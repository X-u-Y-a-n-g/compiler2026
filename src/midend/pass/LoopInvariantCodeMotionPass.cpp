#include "../../../include/compiler/midend/pass/LoopInvariantCodeMotionPass.h"

#include <algorithm>
#include <deque>
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

struct BlockInfo {
  std::string name;
  std::vector<std::size_t> preds;
  std::vector<std::size_t> succs;
};

struct LoopInfo {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::unordered_set<std::size_t> blocks;
};

bool isValueReference(const std::string& operand) {
  return !operand.empty() && operand.front() == '%';
}

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
      if (falseTarget != indexByName.end() && falseTarget != trueTarget) {
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
      if (!reachable[succ]) {
        reachable[succ] = true;
        worklist.push_back(succ);
      }
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

std::vector<int> computeImmediateDominators(const std::vector<BlockInfo>& cfg,
                                            const std::vector<bool>& reachable) {
  const std::size_t n = cfg.size();
  std::vector<int> idom(n, -1);
  if (n == 0) {
    return idom;
  }

  const auto rpo = computeReversePostOrder(cfg, reachable);
  std::vector<std::size_t> rpoIndex(n, n);
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

  return idom;
}

bool dominates(std::size_t dominator, std::size_t block, const std::vector<int>& idom) {
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

std::optional<LoopInfo> buildNaturalLoop(const std::vector<BlockInfo>& cfg, std::size_t header,
                                         std::size_t latch) {
  LoopInfo loop;
  loop.header = header;
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

  std::optional<std::size_t> preheader;
  for (const auto pred : cfg[header].preds) {
    if (loop.blocks.find(pred) != loop.blocks.end()) {
      continue;
    }
    if (preheader.has_value()) {
      return std::nullopt;
    }
    preheader = pred;
  }
  if (!preheader.has_value()) {
    return std::nullopt;
  }

  loop.preheader = *preheader;
  return loop;
}

std::vector<LoopInfo> findLoops(const std::vector<BlockInfo>& cfg, const std::vector<int>& idom,
                                const std::vector<bool>& reachable) {
  std::vector<LoopInfo> loops;
  for (std::size_t latch = 0; latch < cfg.size(); ++latch) {
    if (!reachable[latch]) {
      continue;
    }
    for (const auto header : cfg[latch].succs) {
      if (!reachable[header] || !dominates(header, latch, idom)) {
        continue;
      }
      auto loop = buildNaturalLoop(cfg, header, latch);
      if (loop.has_value()) {
        loops.push_back(std::move(*loop));
      }
    }
  }

  std::sort(loops.begin(), loops.end(), [](const LoopInfo& lhs, const LoopInfo& rhs) {
    return lhs.blocks.size() < rhs.blocks.size();
  });
  return loops;
}

std::unordered_map<std::string, std::size_t> computeDefinitionBlocks(const Function& function) {
  std::unordered_map<std::string, std::size_t> definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult()) {
        definitions[instruction.result()] = block;
      }
    }
  }
  return definitions;
}

bool isHoistableCandidate(const Instruction& instruction) {
  if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
    return false;
  }
  const auto& opcode = instruction.opcode();
  const bool isGep = opcode == "gep" || opcode == "gep.linear" ||
                     opcode == "getelementptr" || opcode == "getelementptr.linear";
  return isGep && !instruction.operands().empty() &&
         !instruction.operands().front().empty() &&
         instruction.operands().front().front() == '@';
}

bool operandsAreLoopInvariant(
    const Instruction& instruction, const LoopInfo& loop,
    const std::unordered_map<std::string, std::size_t>& definitions,
    const std::unordered_set<std::string>& selected) {
  for (const auto& operand : instruction.operands()) {
    if (!isValueReference(operand)) {
      continue;
    }
    const auto definition = definitions.find(operand);
    if (definition == definitions.end()) {
      continue;
    }
    if (loop.blocks.find(definition->second) == loop.blocks.end()) {
      continue;
    }
    if (selected.find(operand) != selected.end()) {
      continue;
    }
    return false;
  }
  return true;
}

bool hoistFromLoop(Function& function, const LoopInfo& loop,
                   const std::unordered_map<std::string, std::size_t>& definitions) {
  std::vector<std::pair<std::size_t, Instruction>> hoisted;
  std::unordered_set<std::string> selected;

  bool found = true;
  while (found) {
    found = false;
    for (const auto block : loop.blocks) {
      for (const auto& instruction : function.blocks()[block].instructions()) {
        if (!isHoistableCandidate(instruction) ||
            selected.find(instruction.result()) != selected.end()) {
          continue;
        }
        if (!operandsAreLoopInvariant(instruction, loop, definitions, selected)) {
          continue;
        }
        selected.insert(instruction.result());
        hoisted.emplace_back(block, instruction);
        found = true;
      }
    }
  }

  if (hoisted.empty()) {
    return false;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && selected.find(instruction.result()) != selected.end()) {
        continue;
      }
      rewritten.push_back(instruction);
    }
    block.instructions() = std::move(rewritten);
  }

  auto& preheaderInstructions = function.blocks()[loop.preheader].instructions();
  auto insertPos = preheaderInstructions.end();
  if (!preheaderInstructions.empty() && preheaderInstructions.back().isTerminator()) {
    insertPos = preheaderInstructions.end() - 1;
  }

  std::vector<Instruction> toInsert;
  toInsert.reserve(hoisted.size());
  for (auto& [unusedBlock, instruction] : hoisted) {
    (void)unusedBlock;
    toInsert.push_back(std::move(instruction));
  }
  preheaderInstructions.insert(insertPos, toInsert.begin(), toInsert.end());
  return true;
}

bool runLicmOnce(Function& function) {
  const auto cfg = buildCFG(function);
  if (cfg.empty()) {
    return false;
  }

  const auto reachable = computeReachable(cfg);
  const auto idom = computeImmediateDominators(cfg, reachable);
  const auto loops = findLoops(cfg, idom, reachable);
  if (loops.empty()) {
    return false;
  }

  bool changed = false;
  const auto definitions = computeDefinitionBlocks(function);
  for (const auto& loop : loops) {
    changed |= hoistFromLoop(function, loop, definitions);
  }
  return changed;
}

void runLicm(Function& function) {
  bool changed = false;
  do {
    changed = runLicmOnce(function);
  } while (changed);
}

}  // namespace

void LoopInvariantCodeMotionPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    runLicm(function);
  }
}

}  // namespace compiler::midend::pass
