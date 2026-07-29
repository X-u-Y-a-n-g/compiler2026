#include "../../../include/compiler/midend/pass/DeadGlobalArrayStoreEliminationPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

bool isPointerProjection(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  return opcode == "cast" || opcode == "getelementptr" ||
         opcode == "getelementptr.linear" || opcode == "gep" ||
         opcode == "gep.linear";
}

std::unordered_set<std::string> derivedPointers(
    const Function& function, const std::string& global) {
  std::unordered_set<std::string> derived{global};
  bool changed = false;
  do {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult() || !isPointerProjection(instruction) ||
            instruction.operands().empty() ||
            derived.find(instruction.operands().front()) == derived.end()) {
          continue;
        }
        changed |= derived.insert(instruction.result()).second;
      }
    }
  } while (changed);
  return derived;
}

bool isOnlyAddressOperand(const Instruction& instruction,
                          std::size_t operandIndex) {
  if (instruction.opcode() != "store" ||
      operandIndex + 1 != instruction.operands().size()) {
    return false;
  }
  for (std::size_t index = 0; index + 1 < instruction.operands().size();
       ++index) {
    if (instruction.operands()[index] ==
        instruction.operands()[operandIndex]) {
      return false;
    }
  }
  return true;
}

bool analyzeObject(
    const Module& module, const std::string& global,
    std::unordered_set<const Instruction*>& removableStores) {
  std::unordered_set<const Instruction*> stores;
  for (const auto& function : module.functions()) {
    const auto derived = derivedPointers(function, global);
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        const auto& operands = instruction.operands();
        for (std::size_t index = 0; index < operands.size(); ++index) {
          if (derived.find(operands[index]) == derived.end()) continue;
          if (isPointerProjection(instruction) && index == 0) continue;
          if (isOnlyAddressOperand(instruction, index)) {
            stores.insert(&instruction);
            continue;
          }
          return false;
        }
      }
    }
  }
  if (stores.empty()) return false;
  removableStores.insert(stores.begin(), stores.end());
  return true;
}

}  // namespace

void DeadGlobalArrayStoreEliminationPass::run(Module& module) {
  if (std::getenv("COMPILER_DISABLE_DEAD_GLOBAL_ARRAY_STORES") != nullptr) {
    return;
  }

  std::unordered_set<const Instruction*> removableStores;
  std::size_t eliminatedObjects = 0;
  for (const auto& global : module.globals()) {
    if (global.isConst() || global.type().find('[') == std::string::npos) {
      continue;
    }
    if (analyzeObject(module, "@" + global.name(), removableStores)) {
      ++eliminatedObjects;
    }
  }

  const std::size_t eliminatedStores = removableStores.size();
  if (!removableStores.empty()) {
    for (auto& function : module.functions()) {
      for (auto& block : function.blocks()) {
        std::vector<Instruction> kept;
        kept.reserve(block.instructions().size());
        for (const auto& instruction : block.instructions()) {
          if (removableStores.find(&instruction) == removableStores.end()) {
            kept.push_back(instruction);
          }
        }
        block.instructions() = std::move(kept);
      }
    }
  }

  if (std::getenv("COMPILER_DEAD_GLOBAL_ARRAY_STORE_STATS") != nullptr) {
    std::cerr << "[dead-global-array-stores] objects=" << eliminatedObjects
              << " stores=" << eliminatedStores << '\n';
  }
}

}  // namespace compiler::midend::pass
