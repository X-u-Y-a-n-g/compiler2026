#include "../../../include/compiler/midend/pass/RedundantStoreEliminationPass.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::midend::analysis::FunctionEffectAnalysis;

std::string calledFunctionName(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& callee = instruction.operands().front();
  if (callee.size() < 2 || callee.front() != '@') {
    return {};
  }
  return callee.substr(1);
}

void eliminateRedundantStores(Function& function,
                              const FunctionEffectAnalysis& functionEffects) {
  for (auto& block : function.blocks()) {
    std::unordered_map<std::string, std::string> knownMemoryValues;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          operands.size() == 2) {
        knownMemoryValues[operands[1]] = instruction.result();
        rewritten.push_back(instruction);
        continue;
      }

      if (instruction.opcode() == "store" && operands.size() == 3) {
        const auto& value = operands[1];
        const auto& address = operands[2];
        const auto known = knownMemoryValues.find(address);
        const bool redundant =
            known != knownMemoryValues.end() && known->second == value;

        for (auto iterator = knownMemoryValues.begin();
             iterator != knownMemoryValues.end();) {
          if (iterator->second != value) {
            iterator = knownMemoryValues.erase(iterator);
          } else {
            ++iterator;
          }
        }
        knownMemoryValues[address] = value;
        if (!redundant) {
          rewritten.push_back(instruction);
        }
        continue;
      }

      if (instruction.opcode() == "call") {
        const auto& effects =
            functionEffects.effectFor(calledFunctionName(instruction));
        if (effects.writesExternalMemory || effects.hasOtherSideEffects) {
          knownMemoryValues.clear();
        }
      }
      rewritten.push_back(instruction);
    }
    block.instructions() = std::move(rewritten);
  }
}

}  // namespace

void RedundantStoreEliminationPass::run(compiler::ir::core::Module& module) {
  const FunctionEffectAnalysis functionEffects(module);
  for (auto& function : module.functions()) {
    eliminateRedundantStores(function, functionEffects);
  }
}

}  // namespace compiler::midend::pass
