#include "../../../include/compiler/midend/pass/DeadCodeEliminationPass.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

void collectUses(const Function& function, std::unordered_map<std::string, std::size_t>& uses) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (!operand.empty() && operand.front() == '%') {
          ++uses[operand];
        }
      }
    }
  }
}

void eliminateDeadInstructions(Function& function) {
  std::unordered_map<std::string, std::size_t> uses;
  collectUses(function, uses);

  bool removedInIteration = true;
  while (removedInIteration) {
    removedInIteration = false;
    for (auto& block : function.blocks()) {
      std::vector<Instruction> kept;
      kept.reserve(block.instructions().size());

      for (const auto& instruction : block.instructions()) {
        if (instruction.hasResult() && detail::isSideEffectFree(instruction) &&
            uses[instruction.result()] == 0) {
          for (const auto& operand : instruction.operands()) {
            if (!operand.empty() && operand.front() == '%' && uses[operand] > 0) {
              --uses[operand];
            }
          }
          removedInIteration = true;
          continue;
        }
        kept.push_back(instruction);
      }

      block.instructions() = std::move(kept);
    }
  }
}

}  // namespace

void DeadCodeEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    eliminateDeadInstructions(function);
  }
}

}  // namespace compiler::midend::pass
