#include "../../../include/compiler/backend/opt/MachineLICM.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;

bool dominates(std::size_t dominator, std::size_t block,
               const std::vector<std::unordered_set<std::size_t>>& doms) {
  return doms[block].find(dominator) != doms[block].end();
}

std::vector<std::unordered_set<std::size_t>> computeDominators(
    const compiler::backend::common::ControlFlowGraph& cfg) {
  const std::size_t count = cfg.blocks.size();
  std::vector<std::unordered_set<std::size_t>> doms(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (i == 0) {
      doms[i].insert(i);
    } else {
      for (std::size_t j = 0; j < count; ++j) {
        doms[i].insert(j);
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      std::unordered_set<std::size_t> next;
      bool firstPred = true;
      for (const auto pred : cfg.predecessors[block]) {
        if (firstPred) {
          next = doms[pred];
          firstPred = false;
          continue;
        }
        for (auto it = next.begin(); it != next.end();) {
          if (doms[pred].find(*it) == doms[pred].end()) {
            it = next.erase(it);
          } else {
            ++it;
          }
        }
      }
      next.insert(block);
      if (next != doms[block]) {
        doms[block] = std::move(next);
        changed = true;
      }
    }
  }
  return doms;
}

std::unordered_set<std::size_t> naturalLoop(
    std::size_t header, std::size_t latch,
    const compiler::backend::common::ControlFlowGraph& cfg) {
  std::unordered_set<std::size_t> loop;
  std::vector<std::size_t> stack;
  loop.insert(header);
  loop.insert(latch);
  stack.push_back(latch);
  while (!stack.empty()) {
    const auto block = stack.back();
    stack.pop_back();
    for (const auto pred : cfg.predecessors[block]) {
      if (loop.insert(pred).second && pred != header) {
        stack.push_back(pred);
      }
    }
  }
  return loop;
}

std::size_t insertionPoint(const compiler::backend::mir::MachineBlock& block,
                           const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::size_t index = block.instructions.size();
  while (index > 0) {
    const auto& instruction = block.instructions[index - 1];
    if (adapter.isReturn(instruction) || adapter.isConditionalBranch(instruction) ||
        adapter.isUnconditionalBranch(instruction)) {
      --index;
      continue;
    }
    break;
  }
  return index;
}

bool canHoist(const MachineInstruction& instruction,
              const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (!detail::isPureInstruction(instruction, adapter) ||
      instruction.kind != MachineInstructionKind::Target ||
      !detail::isVirtualRegister(instruction.result)) {
    return false;
  }
  return instruction.opcode != "div" && instruction.opcode != "sdiv" &&
         instruction.opcode != "mod" && instruction.opcode != "srem" &&
         instruction.opcode != "fdiv" && instruction.opcode != "load" &&
         instruction.opcode != "store" && instruction.opcode.rfind("fcmp.", 0) != 0;
}

bool operandsInvariant(const MachineInstruction& instruction,
                       const std::unordered_set<std::string>& loopDefs,
                       const compiler::backend::target::TargetInstrAdapter& adapter) {
  for (const auto& use : detail::usesOf(instruction, adapter)) {
    const auto key = detail::regKey(use);
    if (!key.empty() && loopDefs.find(key) != loopDefs.end()) {
      return false;
    }
  }
  return true;
}

}  // namespace

void MachineLICM::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    if (function.blocks.size() < 2) {
      continue;
    }

    const auto cfg = compiler::backend::common::CFGBuilder{}.build(function, adapter);
    const auto doms = computeDominators(cfg);

    for (std::size_t latch = 0; latch < function.blocks.size(); ++latch) {
      for (const auto header : cfg.successors[latch]) {
        if (!dominates(header, latch, doms)) {
          continue;
        }

        auto loop = naturalLoop(header, latch, cfg);
        std::vector<std::size_t> outsidePreds;
        for (const auto pred : cfg.predecessors[header]) {
          if (loop.find(pred) == loop.end()) {
            outsidePreds.push_back(pred);
          }
        }
        if (outsidePreds.size() != 1) {
          continue;
        }
        if (loop.size() != 1 || loop.find(header) == loop.end()) {
          continue;
        }
        const auto preheader = outsidePreds.front();

        std::unordered_set<std::string> loopDefs;
        for (const auto blockIndex : loop) {
          for (const auto& instruction : function.blocks[blockIndex].instructions) {
            for (const auto& def : detail::defsOf(instruction, adapter)) {
              const auto key = detail::regKey(def);
              if (!key.empty()) {
                loopDefs.insert(key);
              }
            }
          }
        }

        std::vector<MachineInstruction> hoisted;
        for (const auto blockIndex : loop) {
          if (blockIndex != header) {
            continue;
          }
          if (blockIndex == preheader) {
            continue;
          }
          auto& block = function.blocks[blockIndex];
          std::vector<bool> removeInstruction(block.instructions.size(), false);
          for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& instruction = block.instructions[i];
            if (!canHoist(instruction, adapter) ||
                !operandsInvariant(instruction, loopDefs, adapter)) {
              continue;
            }
            removeInstruction[i] = true;
            hoisted.push_back(instruction);
            loopDefs.erase(detail::regKey(instruction.result));
          }
          detail::removeDeadInstructionsByMask(block, removeInstruction);
        }

        if (!hoisted.empty()) {
          auto& preheaderBlock = function.blocks[preheader];
          const auto insertAt = insertionPoint(preheaderBlock, adapter);
          preheaderBlock.instructions.insert(preheaderBlock.instructions.begin() + insertAt,
                                             hoisted.begin(), hoisted.end());
        }
      }
    }
  }
}

}  // namespace compiler::backend::opt
