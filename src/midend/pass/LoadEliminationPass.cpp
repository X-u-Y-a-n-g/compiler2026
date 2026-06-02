#include "../../../include/compiler/midend/pass/LoadEliminationPass.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRPassUtils.h"
#include "MemoryAccessUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct AvailableLoad {
  std::string type;
  std::string value;
};

using LoadMap = std::unordered_map<std::string, AvailableLoad>;

LoadMap rewriteBlock(BasicBlock& block,
                     const std::unordered_map<std::string, Instruction>& definitions,
                     LoadMap availableLoads) {
  std::unordered_map<std::string, std::string> aliases;
  std::vector<Instruction> rewritten;
  rewritten.reserve(block.instructions().size());

  for (const auto& instruction : block.instructions()) {
    auto operands = detail::rewriteOperands(instruction.operands(), aliases);
    Instruction current{instruction.opcode(), std::move(operands), instruction.result()};

    if (memory::isLoad(current) && current.hasResult()) {
      const auto address = memory::loadAddress(current, definitions);
      if (address.kind != memory::AddressClass::MayAlias) {
        const auto found = availableLoads.find(address.key);
        if (found != availableLoads.end() && found->second.type == current.operands()[0]) {
          aliases[current.result()] = detail::resolveAlias(found->second.value, aliases);
          continue;
        }
        availableLoads[address.key] = AvailableLoad{current.operands()[0], current.result()};
      } else {
        availableLoads.clear();
      }
    } else if (memory::isStore(current)) {
      memory::invalidateForWrite(availableLoads, memory::storeAddress(current, definitions));
    } else if (current.opcode() == "call") {
      availableLoads.clear();
    }

    if (current.hasResult() && !memory::isLoad(current)) {
      aliases.erase(current.result());
    }
    rewritten.push_back(std::move(current));
  }

  block.instructions() = std::move(rewritten);
  return availableLoads;
}

void runLoadElimination(Function& function) {
  const auto definitions = memory::collectDefinitions(function);
  const auto predecessors = memory::computePredecessors(function);
  const auto successorCounts = memory::computeSuccessorCounts(function);
  std::unordered_map<std::string, LoadMap> exitLoads;
  for (auto& block : function.blocks()) {
    LoadMap incomingLoads;
    const auto predIt = predecessors.find(block.name());
    if (predIt != predecessors.end() && predIt->second.size() == 1) {
      const auto& pred = predIt->second.front();
      const auto succCountIt = successorCounts.find(pred);
      const auto exitIt = exitLoads.find(pred);
      if (succCountIt != successorCounts.end() && succCountIt->second == 1 &&
          exitIt != exitLoads.end()) {
        incomingLoads = exitIt->second;
      }
    }
    exitLoads[block.name()] = rewriteBlock(block, definitions, std::move(incomingLoads));
  }
}

}  // namespace

bool LoadEliminationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runLoadElimination(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
