#include "../../../include/compiler/midend/pass/Mem2RegPass.h"

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

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct CFG {
  std::unordered_map<std::string, std::size_t> indexByName;
  std::vector<std::string> names;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
};

struct SlotInfo {
  std::string type;
  std::size_t allocaBlock = 0;
  bool escaped = false;
  std::unordered_set<std::size_t> defBlocks;
  std::unordered_set<std::size_t> useBeforeDefBlocks;
};

struct DominatorInfo {
  std::vector<std::unordered_set<std::size_t>> dominators;
  std::vector<int> idom;
  std::vector<std::vector<std::size_t>> children;
  std::vector<std::unordered_set<std::size_t>> frontier;
};

bool isScalarAlloca(const Instruction &instruction) {
  return instruction.opcode() == "alloca" && instruction.hasResult() &&
         instruction.operands().size() == 1 &&
         instruction.operands().front().find('[') == std::string::npos;
}

std::string zeroValueForType(const std::string &type) {
  return type == "float" ? "0.0" : "0";
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

CFG buildCFG(const Function &function) {
  CFG cfg;
  cfg.names.reserve(function.blocks().size());
  for (const auto &block : function.blocks()) {
    cfg.indexByName.emplace(block.name(), cfg.names.size());
    cfg.names.push_back(block.name());
  }

  cfg.successors.resize(cfg.names.size());
  cfg.predecessors.resize(cfg.names.size());
  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    for (const auto &target : branchTargets(function.blocks()[i])) {
      const auto found = cfg.indexByName.find(target);
      if (found == cfg.indexByName.end()) {
        continue;
      }
      cfg.successors[i].push_back(found->second);
      cfg.predecessors[found->second].push_back(i);
    }
  }
  return cfg;
}

std::vector<bool> computeReachable(const CFG &cfg) {
  std::vector<bool> reachable(cfg.names.size(), false);
  if (cfg.names.empty()) {
    return reachable;
  }
  std::deque<std::size_t> worklist;
  reachable[0] = true;
  worklist.push_back(0);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto succ : cfg.successors[block]) {
      if (reachable[succ]) {
        continue;
      }
      reachable[succ] = true;
      worklist.push_back(succ);
    }
  }
  return reachable;
}

std::vector<std::size_t>
computeReversePostOrder(const CFG &cfg, const std::vector<bool> &reachable) {
  std::vector<std::size_t> order;
  if (cfg.names.empty()) {
    return order;
  }

  std::vector<bool> visited(cfg.names.size(), false);
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  stack.emplace_back(0, 0);
  visited[0] = true;
  while (!stack.empty()) {
    auto &[block, nextSucc] = stack.back();
    if (nextSucc < cfg.successors[block].size()) {
      const auto succ = cfg.successors[block][nextSucc++];
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

std::size_t intersectIdoms(std::size_t lhs, std::size_t rhs,
                           const std::vector<int> &idom,
                           const std::vector<std::size_t> &rpoIndex) {
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

DominatorInfo computeDominators(const CFG &cfg) {
  DominatorInfo info;
  const std::size_t count = cfg.names.size();
  info.idom.assign(count, -1);
  info.children.resize(count);
  info.frontier.resize(count);
  if (count == 0) {
    return info;
  }

  const auto reachable = computeReachable(cfg);
  const auto rpo = computeReversePostOrder(cfg, reachable);
  std::vector<std::size_t> rpoIndex(count, count);
  for (std::size_t i = 0; i < rpo.size(); ++i) {
    rpoIndex[rpo[i]] = i;
  }

  info.idom[0] = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto block : rpo) {
      if (block == 0) {
        continue;
      }
      std::optional<std::size_t> next;
      for (const auto pred : cfg.predecessors[block]) {
        if (!reachable[pred] || info.idom[pred] < 0) {
          continue;
        }
        if (!next.has_value()) {
          next = pred;
        } else {
          next = intersectIdoms(pred, *next, info.idom, rpoIndex);
        }
      }
      if (next.has_value() && info.idom[block] != static_cast<int>(*next)) {
        info.idom[block] = static_cast<int>(*next);
        changed = true;
      }
    }
  }

  info.idom[0] = -1;
  for (std::size_t block = 1; block < count; ++block) {
    if (!reachable[block] || info.idom[block] < 0) {
      continue;
    }
    info.children[static_cast<std::size_t>(info.idom[block])].push_back(block);
  }

  for (std::size_t block = 0; block < count; ++block) {
    if (!reachable[block] || cfg.predecessors[block].size() < 2) {
      continue;
    }
    for (const auto pred : cfg.predecessors[block]) {
      if (!reachable[pred]) {
        continue;
      }
      int runner = static_cast<int>(pred);
      while (runner >= 0 && runner != info.idom[block]) {
        info.frontier[static_cast<std::size_t>(runner)].insert(block);
        runner = info.idom[static_cast<std::size_t>(runner)];
      }
    }
  }

  return info;
}

bool dominates(std::size_t dominator, std::size_t block,
               const DominatorInfo &dom) {
  if (dominator == block) {
    return true;
  }
  int runner = static_cast<int>(block);
  while (runner >= 0) {
    runner = dom.idom[static_cast<std::size_t>(runner)];
    if (runner == static_cast<int>(dominator)) {
      return true;
    }
  }
  return false;
}

std::unordered_map<std::string, SlotInfo>
collectSlotInfo(const Function &function, const CFG &cfg,
                const DominatorInfo &dom) {
  std::unordered_map<std::string, SlotInfo> slots;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    if (blockIndex >= dom.idom.size() ||
        (blockIndex != 0 && dom.idom[blockIndex] < 0)) {
      continue;
    }
    for (const auto &instruction :
         function.blocks()[blockIndex].instructions()) {
      if (isScalarAlloca(instruction)) {
        slots.emplace(
            instruction.result(),
            SlotInfo{
                instruction.operands().front(), blockIndex, false, {}, {}});
      }
    }
  }

  if (slots.empty()) {
    return slots;
  }

  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    std::unordered_set<std::string> definedInBlock;
    const auto &block = function.blocks()[blockIndex];
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      if (instruction.opcode() == "load" && operands.size() == 2) {
        auto found = slots.find(operands[1]);
        if (found != slots.end() &&
            !dominates(found->second.allocaBlock, blockIndex, dom)) {
          found->second.escaped = true;
        }
        if (found != slots.end() &&
            definedInBlock.find(operands[1]) == definedInBlock.end()) {
          found->second.useBeforeDefBlocks.insert(blockIndex);
        }
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3) {
        if (auto found = slots.find(operands[2]); found != slots.end()) {
          if (!dominates(found->second.allocaBlock, blockIndex, dom)) {
            found->second.escaped = true;
          }
          found->second.defBlocks.insert(blockIndex);
          definedInBlock.insert(operands[2]);
        }
        if (auto storedAddress = slots.find(operands[1]);
            storedAddress != slots.end()) {
          storedAddress->second.escaped = true;
        }
        continue;
      }

      for (const auto &operand : operands) {
        if (auto found = slots.find(operand); found != slots.end()) {
          if (!dominates(found->second.allocaBlock, blockIndex, dom)) {
            found->second.escaped = true;
          }
          found->second.escaped = true;
        }
      }
    }
  }

  (void)cfg;
  return slots;
}

std::unordered_set<std::size_t> computeLiveInBlocks(const SlotInfo &slot,
                                                    const CFG &cfg) {
  std::unordered_set<std::size_t> liveIn = slot.useBeforeDefBlocks;
  std::deque<std::size_t> worklist(slot.useBeforeDefBlocks.begin(),
                                   slot.useBeforeDefBlocks.end());

  while (!worklist.empty()) {
    const std::size_t block = worklist.front();
    worklist.pop_front();
    for (const auto pred : cfg.predecessors[block]) {
      if (liveIn.find(pred) != liveIn.end()) {
        continue;
      }
      liveIn.insert(pred);
      if (slot.defBlocks.find(pred) == slot.defBlocks.end()) {
        worklist.push_back(pred);
      }
    }
  }

  return liveIn;
}

struct PhiPlacement {
  std::unordered_map<std::string, std::string> resultToSlot;
};

PhiPlacement
insertPhiNodes(Function &function, const CFG &cfg, const DominatorInfo &dom,
               const std::unordered_map<std::string, SlotInfo> &slots) {
  PhiPlacement placement;
  std::size_t nextValueIndex = 0;
  std::unordered_set<std::string> usedNames;
  for (const auto &parameter : function.parameters()) {
    usedNames.insert(parameter.name);
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        usedNames.insert(instruction.result());
      }
    }
  }

  auto fresh = [&]() {
    while (true) {
      const std::string candidate = "%m2r" + std::to_string(nextValueIndex++);
      if (usedNames.insert(candidate).second) {
        return candidate;
      }
    }
  };

  for (const auto &[slot, info] : slots) {
    if (info.escaped || info.defBlocks.empty()) {
      continue;
    }

    const auto liveInBlocks = computeLiveInBlocks(info, cfg);
    std::deque<std::size_t> worklist(info.defBlocks.begin(),
                                     info.defBlocks.end());
    std::unordered_set<std::size_t> hasPhi;

    while (!worklist.empty()) {
      const std::size_t block = worklist.front();
      worklist.pop_front();
      for (const auto frontierBlock : dom.frontier[block]) {
        if (liveInBlocks.find(frontierBlock) == liveInBlocks.end()) {
          continue;
        }
        if (!hasPhi.insert(frontierBlock).second) {
          continue;
        }
        const std::string result = fresh();
        auto &instructions = function.blocks()[frontierBlock].instructions();
        instructions.insert(instructions.begin(),
                            Instruction{"phi", {info.type}, result});
        placement.resultToSlot.emplace(result, slot);
        if (info.defBlocks.find(frontierBlock) == info.defBlocks.end()) {
          worklist.push_back(frontierBlock);
        }
      }
    }
  }

  return placement;
}

struct RenameState {
  const CFG &cfg;
  const DominatorInfo &dom;
  const std::unordered_map<std::string, SlotInfo> &slots;
  const PhiPlacement &placement;
  std::unordered_map<std::string, std::vector<std::string>> valueStack;
  std::unordered_map<std::string, std::string> aliases;
};

bool isPromotableSlot(const std::string &slot,
                      const std::unordered_map<std::string, SlotInfo> &slots) {
  const auto found = slots.find(slot);
  return found != slots.end() && !found->second.escaped;
}

std::string currentValueForSlot(const std::string &slot, RenameState &state) {
  auto &stack = state.valueStack[slot];
  if (!stack.empty()) {
    return stack.back();
  }
  const auto found = state.slots.find(slot);
  if (found == state.slots.end()) {
    return "0";
  }
  return zeroValueForType(found->second.type);
}

void addIncomingToSuccessorPhis(Function &function, std::size_t blockIndex,
                                RenameState &state) {
  for (const auto succ : state.cfg.successors[blockIndex]) {
    auto &instructions = function.blocks()[succ].instructions();
    for (auto &instruction : instructions) {
      if (instruction.opcode() != "phi") {
        break;
      }
      const auto slotIt =
          state.placement.resultToSlot.find(instruction.result());
      if (slotIt == state.placement.resultToSlot.end()) {
        continue;
      }
      const std::string value = currentValueForSlot(slotIt->second, state);
      auto operands = instruction.operands();
      operands.push_back(detail::resolveAlias(value, state.aliases));
      operands.push_back(state.cfg.names[blockIndex]);
      instruction =
          Instruction{"phi", std::move(operands), instruction.result()};
    }
  }
}

void renameBlock(Function &function, std::size_t blockIndex,
                 RenameState &state) {
  std::vector<std::string> pushedSlots;
  auto &block = function.blocks()[blockIndex];
  std::vector<Instruction> rewritten;
  rewritten.reserve(block.instructions().size());

  for (const auto &instruction : block.instructions()) {
    if (instruction.opcode() != "phi") {
      break;
    }
    const auto found = state.placement.resultToSlot.find(instruction.result());
    if (found == state.placement.resultToSlot.end()) {
      rewritten.push_back(instruction);
      continue;
    }
    state.valueStack[found->second].push_back(instruction.result());
    pushedSlots.push_back(found->second);
    rewritten.push_back(instruction);
  }

  for (const auto &instruction : block.instructions()) {
    if (instruction.opcode() == "phi") {
      continue;
    }

    auto operands =
        detail::rewriteOperands(instruction.operands(), state.aliases);
    Instruction rewrittenInstruction{instruction.opcode(), operands,
                                     instruction.result()};

    if (rewrittenInstruction.opcode() == "alloca" &&
        isPromotableSlot(rewrittenInstruction.result(), state.slots)) {
      continue;
    }

    if (rewrittenInstruction.opcode() == "load" &&
        rewrittenInstruction.hasResult() && operands.size() == 2 &&
        isPromotableSlot(operands[1], state.slots)) {
      state.aliases[rewrittenInstruction.result()] = detail::resolveAlias(
          currentValueForSlot(operands[1], state), state.aliases);
      continue;
    }

    if (rewrittenInstruction.opcode() == "store" && operands.size() == 3 &&
        isPromotableSlot(operands[2], state.slots)) {
      state.valueStack[operands[2]].push_back(operands[1]);
      pushedSlots.push_back(operands[2]);
      continue;
    }

    if (rewrittenInstruction.hasResult()) {
      state.aliases.erase(rewrittenInstruction.result());
    }
    rewritten.push_back(std::move(rewrittenInstruction));
  }

  block.instructions() = std::move(rewritten);
  addIncomingToSuccessorPhis(function, blockIndex, state);

  for (const auto child : state.dom.children[blockIndex]) {
    renameBlock(function, child, state);
  }

  for (auto it = pushedSlots.rbegin(); it != pushedSlots.rend(); ++it) {
    auto &stack = state.valueStack[*it];
    if (!stack.empty()) {
      stack.pop_back();
    }
  }
}

void runMem2Reg(Function &function) {
  if (function.blocks().empty()) {
    return;
  }

  const CFG cfg = buildCFG(function);
  const DominatorInfo dom = computeDominators(cfg);
  const auto slots = collectSlotInfo(function, cfg, dom);
  bool hasPromotableSlot = false;
  for (const auto &[slot, info] : slots) {
    if (!info.escaped) {
      hasPromotableSlot = true;
      break;
    }
  }
  if (!hasPromotableSlot) {
    return;
  }

  const PhiPlacement placement = insertPhiNodes(function, cfg, dom, slots);
  RenameState state{cfg, dom, slots, placement, {}, {}};
  renameBlock(function, 0, state);
}

} // namespace

bool Mem2RegPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    runMem2Reg(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
