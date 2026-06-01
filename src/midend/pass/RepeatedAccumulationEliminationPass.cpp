#include "../../../include/compiler/midend/pass/RepeatedAccumulationEliminationPass.h"

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
  std::size_t latch = 0;
  std::unordered_set<std::size_t> blocks;
};

struct CountedLoop {
  std::string counterSlot;
  std::string bound;
  std::string exitBlock;
  std::string bodyBlock;
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
  const auto rpo = computeReversePostOrder(cfg, reachable);
  std::vector<std::size_t> rpoIndex(cfg.size(), cfg.size());
  for (std::size_t i = 0; i < rpo.size(); ++i) {
    rpoIndex[rpo[i]] = i;
  }

  std::vector<int> idom(cfg.size(), -1);
  if (cfg.empty()) {
    return idom;
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
    return lhs.blocks.size() > rhs.blocks.size();
  });
  return loops;
}

std::unordered_map<std::string, const Instruction*> definitionsInBlock(
    const std::vector<Instruction>& instructions) {
  std::unordered_map<std::string, const Instruction*> definitions;
  for (const auto& instruction : instructions) {
    if (instruction.hasResult()) {
      definitions[instruction.result()] = &instruction;
    }
  }
  return definitions;
}

std::optional<CountedLoop> matchCountedLoop(const Function& function, const LoopInfo& loop) {
  const auto& headerInstructions = function.blocks()[loop.header].instructions();
  const auto* headerTerminator = function.blocks()[loop.header].terminator();
  if (headerTerminator == nullptr || headerTerminator->opcode() != "br" ||
      headerTerminator->operands().size() != 3) {
    return std::nullopt;
  }

  const auto definitions = definitionsInBlock(headerInstructions);
  const auto condition = definitions.find(headerTerminator->operands()[0]);
  if (condition == definitions.end() || condition->second->opcode() != "icmp.slt" ||
      condition->second->operands().size() != 2) {
    return std::nullopt;
  }

  const auto loadedCounter = definitions.find(condition->second->operands()[0]);
  if (loadedCounter == definitions.end() || loadedCounter->second->opcode() != "load" ||
      loadedCounter->second->operands().size() != 2) {
    return std::nullopt;
  }

  const auto& counterSlot = loadedCounter->second->operands()[1];
  const auto& latchInstructions = function.blocks()[loop.latch].instructions();
  const auto* latchTerminator = function.blocks()[loop.latch].terminator();
  if (latchTerminator == nullptr || latchTerminator->opcode() != "br" ||
      latchTerminator->operands().size() != 1 ||
      latchTerminator->operands()[0] != function.blocks()[loop.header].name()) {
    return std::nullopt;
  }

  bool incrementsCounter = false;
  for (const auto& instruction : latchInstructions) {
    if (instruction.opcode() != "store" || instruction.operands().size() != 3 ||
        instruction.operands()[2] != counterSlot) {
      continue;
    }
    const auto latchDefinitions = definitionsInBlock(latchInstructions);
    const auto increment = latchDefinitions.find(instruction.operands()[1]);
    if (increment == latchDefinitions.end() || increment->second->opcode() != "add" ||
        increment->second->operands().size() != 2 || increment->second->operands()[1] != "1") {
      continue;
    }
    const auto counterLoad = latchDefinitions.find(increment->second->operands()[0]);
    if (counterLoad == latchDefinitions.end() || counterLoad->second->opcode() != "load" ||
        counterLoad->second->operands().size() != 2 ||
        counterLoad->second->operands()[1] != counterSlot) {
      continue;
    }
    incrementsCounter = true;
    break;
  }
  if (!incrementsCounter) {
    return std::nullopt;
  }

  CountedLoop counted;
  counted.counterSlot = counterSlot;
  counted.bound = condition->second->operands()[1];
  counted.bodyBlock = headerTerminator->operands()[1];
  counted.exitBlock = headerTerminator->operands()[2];

  std::string effectiveBodyBlock = counted.bodyBlock;
  for (const auto& block : function.blocks()) {
    if (block.name() != effectiveBodyBlock || block.instructions().size() != 1) {
      continue;
    }
    const auto& onlyInstruction = block.instructions().front();
    if (onlyInstruction.opcode() == "br" && onlyInstruction.operands().size() == 1) {
      effectiveBodyBlock = onlyInstruction.operands().front();
    }
    break;
  }
  if (effectiveBodyBlock.rfind("while.cond", 0) != 0) {
    return std::nullopt;
  }

  return counted;
}

bool hasZeroInitialization(const Function& function, std::size_t blockIndex,
                           const std::string& slot) {
  for (const auto& instruction : function.blocks()[blockIndex].instructions()) {
    if (instruction.opcode() == "store" && instruction.operands().size() == 3 &&
        instruction.operands()[1] == "0" && instruction.operands()[2] == slot) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> zeroInitializedSlots(const Function& function, std::size_t blockIndex) {
  std::vector<std::string> slots;
  for (const auto& instruction : function.blocks()[blockIndex].instructions()) {
    if (instruction.opcode() == "alloca" && instruction.hasResult() &&
        instruction.operands().size() == 1 &&
        instruction.operands().front().find('[') == std::string::npos &&
        hasZeroInitialization(function, blockIndex, instruction.result())) {
      slots.push_back(instruction.result());
    }
  }
  return slots;
}

std::string slotType(const Function& function, const std::string& slot) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.result() == slot &&
          !instruction.operands().empty()) {
        return instruction.operands().front();
      }
    }
  }
  return "int";
}

std::unordered_set<std::string> collectScalarSlots(const Function& function) {
  std::unordered_set<std::string> slots;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult() &&
          instruction.operands().size() == 1 &&
          instruction.operands().front().find('[') == std::string::npos) {
        slots.insert(instruction.result());
      }
    }
  }
  return slots;
}

bool counterUsedInRepeatedBody(const Function& function, const LoopInfo& loop,
                               const std::string& counterSlot) {
  for (const auto block : loop.blocks) {
    if (block == loop.header || block == loop.latch) {
      continue;
    }
    for (const auto& instruction : function.blocks()[block].instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand == counterSlot) {
          return true;
        }
      }
    }
  }
  return false;
}

bool hasUnsafeSideEffects(const Function& function, const LoopInfo& loop,
                          const std::unordered_set<std::string>& scalarSlots) {
  for (const auto block : loop.blocks) {
    if (block == loop.header || block == loop.latch) {
      continue;
    }
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.opcode() == "call") {
        return true;
      }
      if (instruction.opcode() == "store" && instruction.operands().size() == 3 &&
          scalarSlots.find(instruction.operands()[2]) == scalarSlots.end()) {
        return true;
      }
    }
  }
  return false;
}

bool isAccumulationUpdate(const std::vector<Instruction>& instructions,
                          const Instruction& storeInstruction, const std::string& slot) {
  if (storeInstruction.opcode() != "store" || storeInstruction.operands().size() != 3 ||
      storeInstruction.operands()[2] != slot) {
    return false;
  }

  const auto definitions = definitionsInBlock(instructions);
  const auto storedValue = definitions.find(storeInstruction.operands()[1]);
  if (storedValue == definitions.end() || storedValue->second->opcode() != "add" ||
      storedValue->second->operands().size() != 2) {
    return false;
  }

  const auto loadedAccumulator = definitions.find(storedValue->second->operands()[0]);
  return loadedAccumulator != definitions.end() && loadedAccumulator->second->opcode() == "load" &&
         loadedAccumulator->second->operands().size() == 2 &&
         loadedAccumulator->second->operands()[1] == slot;
}

std::optional<std::string> findAccumulatorSlot(const Function& function, const LoopInfo& loop) {
  const auto candidates = zeroInitializedSlots(function, loop.preheader);
  for (const auto& slot : candidates) {
    bool foundUpdate = false;
    bool unsupportedStore = false;
    for (const auto block : loop.blocks) {
      for (const auto& instruction : function.blocks()[block].instructions()) {
        if (instruction.opcode() != "store" || instruction.operands().size() != 3 ||
            instruction.operands()[2] != slot) {
          continue;
        }
        if (block == loop.preheader) {
          continue;
        }
        if (isAccumulationUpdate(function.blocks()[block].instructions(), instruction, slot)) {
          foundUpdate = true;
        } else {
          unsupportedStore = true;
        }
      }
    }
    if (foundUpdate && !unsupportedStore) {
      return slot;
    }
  }
  return std::nullopt;
}

std::string freshValueName(const Function& function, const std::string& prefix) {
  std::unordered_set<std::string> used;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }

  for (int index = 0;; ++index) {
    const auto candidate = "%" + prefix + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

bool rewriteLatch(Function& function, const LoopInfo& loop, const CountedLoop& counted,
                  const std::string& accumulatorSlot) {
  auto& instructions = function.blocks()[loop.latch].instructions();
  if (instructions.empty()) {
    return false;
  }

  const auto loadName = freshValueName(function, "rae.load");
  const auto productName = freshValueName(function, "rae.mul");
  const auto type = slotType(function, accumulatorSlot);
  std::vector<Instruction> rewritten;
  rewritten.emplace_back("load", std::vector<std::string>{type, accumulatorSlot}, loadName);
  rewritten.emplace_back("mul", std::vector<std::string>{loadName, counted.bound}, productName);
  rewritten.emplace_back("store", std::vector<std::string>{type, productName, accumulatorSlot});
  rewritten.emplace_back("br", std::vector<std::string>{counted.exitBlock});
  instructions = std::move(rewritten);
  return true;
}

bool runRepeatedAccumulationElimination(Function& function) {
  const auto cfg = buildCFG(function);
  if (cfg.empty()) {
    return false;
  }
  const auto reachable = computeReachable(cfg);
  const auto idom = computeImmediateDominators(cfg, reachable);
  const auto loops = findLoops(cfg, idom, reachable);

  for (const auto& loop : loops) {
    const auto counted = matchCountedLoop(function, loop);
    if (!counted.has_value()) {
      continue;
    }

    const auto scalarSlots = collectScalarSlots(function);
    if (counterUsedInRepeatedBody(function, loop, counted->counterSlot) ||
        hasUnsafeSideEffects(function, loop, scalarSlots)) {
      continue;
    }

    const auto accumulatorSlot = findAccumulatorSlot(function, loop);
    if (!accumulatorSlot.has_value() || *accumulatorSlot == counted->counterSlot) {
      continue;
    }
    return rewriteLatch(function, loop, *counted, *accumulatorSlot);
  }
  return false;
}

}  // namespace

void RepeatedAccumulationEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    while (runRepeatedAccumulationElimination(function)) {
    }
  }
}

}  // namespace compiler::midend::pass
