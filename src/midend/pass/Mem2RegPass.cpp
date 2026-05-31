#include "../../../include/compiler/midend/pass/Mem2RegPass.h"

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

struct SlotInfo {
  std::unordered_set<std::string> usedBlocks;
  bool escaped = false;
};

bool isScalarAlloca(const Instruction& instruction) {
  return instruction.opcode() == "alloca" && instruction.hasResult() &&
         instruction.operands().size() == 1 &&
         instruction.operands().front().find('[') == std::string::npos;
}

std::unordered_map<std::string, SlotInfo> collectSlotInfo(const Function& function) {
  std::unordered_map<std::string, SlotInfo> info;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (isScalarAlloca(instruction)) {
        info.emplace(instruction.result(), SlotInfo{});
      }
    }
  }

  if (info.empty()) {
    return info;
  }

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      if (instruction.opcode() == "load" && operands.size() == 2) {
        auto found = info.find(operands[1]);
        if (found != info.end()) {
          found->second.usedBlocks.insert(block.name());
        }
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3) {
        auto found = info.find(operands[2]);
        if (found != info.end()) {
          found->second.usedBlocks.insert(block.name());
        }
        continue;
      }

      for (const auto& operand : operands) {
        auto found = info.find(operand);
        if (found != info.end()) {
          found->second.escaped = true;
        }
      }
    }
  }

  return info;
}

std::unordered_set<std::string> collectPromotableSlots(const Function& function) {
  std::unordered_set<std::string> promotable;
  auto info = collectSlotInfo(function);
  for (const auto& [slot, slotInfo] : info) {
    if (!slotInfo.escaped && slotInfo.usedBlocks.size() <= 1) {
      promotable.insert(slot);
    }
  }
  return promotable;
}

struct BlockPromotionResult {
  std::unordered_set<std::string> removableSlots;
};

BlockPromotionResult promoteBlock(BasicBlock& block,
                                  const std::unordered_set<std::string>& promotableSlots) {
  std::unordered_map<std::string, std::string> replacements;
  std::unordered_map<std::string, std::string> currentValue;
  std::unordered_map<std::string, bool> sawUnreplacedLoad;
  std::unordered_map<std::string, bool> sawAnyLoad;
  std::unordered_set<std::string> touchedSlots;
  std::vector<Instruction> rewrittenInstructions;
  rewrittenInstructions.reserve(block.instructions().size());

  for (const auto& instruction : block.instructions()) {
    auto operands = detail::rewriteOperands(instruction.operands(), replacements);
    Instruction rewritten{instruction.opcode(), operands, instruction.result()};
    bool removed = false;

    if (rewritten.opcode() == "load" && rewritten.hasResult() && operands.size() == 2) {
      const auto& slot = operands[1];
      if (promotableSlots.find(slot) != promotableSlots.end()) {
        sawAnyLoad[slot] = true;
        touchedSlots.insert(slot);
        const auto found = currentValue.find(slot);
        if (found != currentValue.end()) {
          replacements[rewritten.result()] = detail::resolveAlias(found->second, replacements);
          removed = true;
        } else {
          sawUnreplacedLoad[slot] = true;
          replacements.erase(rewritten.result());
        }
      } else {
        replacements.erase(rewritten.result());
      }
    } else if (rewritten.opcode() == "store" && operands.size() == 3) {
      const auto& slot = operands[2];
      if (promotableSlots.find(slot) != promotableSlots.end()) {
        touchedSlots.insert(slot);
        currentValue[slot] = operands[1];
      }
    } else if (rewritten.hasResult()) {
      replacements.erase(rewritten.result());
    }

    if (!removed) {
      rewrittenInstructions.push_back(std::move(rewritten));
    }
  }

  BlockPromotionResult result;
  for (const auto& slot : touchedSlots) {
    if (!sawUnreplacedLoad[slot]) {
      result.removableSlots.insert(slot);
    }
  }

  if (!result.removableSlots.empty()) {
    std::vector<Instruction> kept;
    kept.reserve(rewrittenInstructions.size());
    for (const auto& instruction : rewrittenInstructions) {
      const auto& operands = instruction.operands();
      if (instruction.opcode() == "store" && operands.size() == 3 &&
          result.removableSlots.find(operands[2]) != result.removableSlots.end()) {
        continue;
      }
      if (instruction.hasResult() && instruction.opcode() == "alloca" &&
          result.removableSlots.find(instruction.result()) != result.removableSlots.end()) {
        continue;
      }
      kept.push_back(instruction);
    }
    block.instructions() = std::move(kept);
  } else {
    block.instructions() = std::move(rewrittenInstructions);
  }

  return result;
}

void cleanupPromotedSlots(Function& function,
                          const std::unordered_set<std::string>& removableSlots) {
  if (removableSlots.empty()) {
    return;
  }
  for (auto& block : function.blocks()) {
    std::vector<Instruction> kept;
    kept.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      if (instruction.opcode() == "store" && operands.size() == 3 &&
          removableSlots.find(operands[2]) != removableSlots.end()) {
        continue;
      }
      if (instruction.hasResult() && instruction.opcode() == "alloca" &&
          removableSlots.find(instruction.result()) != removableSlots.end()) {
        continue;
      }
      kept.push_back(instruction);
    }
    block.instructions() = std::move(kept);
  }
}

void runMem2Reg(Function& function) {
  const auto promotable = collectPromotableSlots(function);
  if (promotable.empty()) {
    return;
  }

  std::unordered_set<std::string> removableSlots;
  for (auto& block : function.blocks()) {
    BlockPromotionResult result = promoteBlock(block, promotable);
    removableSlots.insert(result.removableSlots.begin(), result.removableSlots.end());
  }

  cleanupPromotedSlots(function, removableSlots);
}

}  // namespace

void Mem2RegPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    runMem2Reg(function);
  }
}

}  // namespace compiler::midend::pass
