#include "../../../include/compiler/midend/pass/LocalScalarCleanupPass.h"

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

struct PreviousStore {
  bool valid = false;
  std::string type;
  std::string value;
  std::string address;
};

bool isScalarAlloca(const Instruction& instruction) {
  return instruction.opcode() == "alloca" && instruction.hasResult() &&
         instruction.operands().size() == 1 &&
         instruction.operands().front().find('[') == std::string::npos;
}

void forwardAdjacentStoreLoads(BasicBlock& block) {
  std::unordered_map<std::string, std::string> replacements;
  std::vector<Instruction> rewrittenInstructions;
  rewrittenInstructions.reserve(block.instructions().size());

  PreviousStore previousStore;
  for (const auto& instruction : block.instructions()) {
    auto operands = detail::rewriteOperands(instruction.operands(), replacements);
    Instruction rewritten{instruction.opcode(), operands, instruction.result()};

    if (rewritten.opcode() == "load" && rewritten.hasResult() && operands.size() == 2) {
      if (previousStore.valid && previousStore.type == operands[0] &&
          previousStore.address == operands[1]) {
        replacements[rewritten.result()] =
            detail::resolveAlias(previousStore.value, replacements);
        continue;
      }
      replacements.erase(rewritten.result());
    } else if (rewritten.hasResult()) {
      replacements.erase(rewritten.result());
    }

    rewrittenInstructions.push_back(std::move(rewritten));
    const auto& kept = rewrittenInstructions.back();
    if (kept.opcode() == "store" && kept.operands().size() == 3) {
      previousStore = PreviousStore{true, kept.operands()[0], kept.operands()[1],
                                    kept.operands()[2]};
    } else {
      previousStore = {};
    }
  }

  block.instructions() = std::move(rewrittenInstructions);
}

void cleanupUnusedScalarSlots(Function& function) {
  std::unordered_set<std::string> scalarAllocas;
  std::unordered_set<std::string> loadedSlots;
  std::unordered_set<std::string> escapedSlots;

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (isScalarAlloca(instruction)) {
        scalarAllocas.insert(instruction.result());
      }
    }
  }

  if (scalarAllocas.empty()) {
    return;
  }

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      if (instruction.opcode() == "load" && operands.size() == 2 &&
          scalarAllocas.find(operands[1]) != scalarAllocas.end()) {
        loadedSlots.insert(operands[1]);
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3) {
        if (scalarAllocas.find(operands[1]) != scalarAllocas.end()) {
          escapedSlots.insert(operands[1]);
        }
        continue;
      }

      for (const auto& operand : operands) {
        if (scalarAllocas.find(operand) != scalarAllocas.end()) {
          escapedSlots.insert(operand);
        }
      }
    }
  }

  std::unordered_set<std::string> removableSlots;
  for (const auto& slot : scalarAllocas) {
    if (loadedSlots.find(slot) == loadedSlots.end() &&
        escapedSlots.find(slot) == escapedSlots.end()) {
      removableSlots.insert(slot);
    }
  }

  if (removableSlots.empty()) {
    return;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> kept;
    kept.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      if (instruction.hasResult() &&
          removableSlots.find(instruction.result()) != removableSlots.end()) {
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3 &&
          removableSlots.find(operands[2]) != removableSlots.end()) {
        continue;
      }
      kept.push_back(instruction);
    }
    block.instructions() = std::move(kept);
  }
}

void cleanupFunction(Function& function) {
  for (auto& block : function.blocks()) {
    forwardAdjacentStoreLoads(block);
  }
  cleanupUnusedScalarSlots(function);
}

}  // namespace

void LocalScalarCleanupPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    cleanupFunction(function);
  }
}

}  // namespace compiler::midend::pass
