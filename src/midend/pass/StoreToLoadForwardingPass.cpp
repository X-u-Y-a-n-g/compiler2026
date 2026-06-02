#include "../../../include/compiler/midend/pass/StoreToLoadForwardingPass.h"

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

struct StoredValue {
  std::string type;
  std::string value;
};

using StoreMap = std::unordered_map<std::string, StoredValue>;

StoreMap rewriteBlock(BasicBlock& block,
                      const std::unordered_map<std::string, Instruction>& definitions,
                      StoreMap availableStores) {
  std::unordered_map<std::string, std::string> aliases;
  std::vector<Instruction> rewritten;
  rewritten.reserve(block.instructions().size());

  for (const auto& instruction : block.instructions()) {
    auto operands = detail::rewriteOperands(instruction.operands(), aliases);
    Instruction current{instruction.opcode(), std::move(operands), instruction.result()};

    if (memory::isLoad(current) && current.hasResult()) {
      const auto address = memory::loadAddress(current, definitions);
      if (address.kind != memory::AddressClass::MayAlias) {
        const auto found = availableStores.find(address.key);
        if (found != availableStores.end() && found->second.type == current.operands()[0]) {
          aliases[current.result()] = detail::resolveAlias(found->second.value, aliases);
          continue;
        }
      } else {
        availableStores.clear();
      }
    } else if (memory::isStore(current)) {
      const auto address = memory::storeAddress(current, definitions);
      if (address.kind == memory::AddressClass::Exact) {
        availableStores[address.key] = StoredValue{current.operands()[0], current.operands()[1]};
      } else if (address.kind == memory::AddressClass::Derived) {
        availableStores.clear();
        availableStores[address.key] = StoredValue{current.operands()[0], current.operands()[1]};
      } else {
        availableStores.clear();
      }
    } else if (current.opcode() == "call") {
      availableStores.clear();
    }

    if (current.hasResult() && !memory::isLoad(current)) {
      aliases.erase(current.result());
    }
    rewritten.push_back(std::move(current));
  }

  block.instructions() = std::move(rewritten);
  return availableStores;
}

void runStoreToLoadForwarding(Function& function) {
  const auto definitions = memory::collectDefinitions(function);
  const auto predecessors = memory::computePredecessors(function);
  const auto successorCounts = memory::computeSuccessorCounts(function);
  std::unordered_map<std::string, StoreMap> exitStores;
  for (auto& block : function.blocks()) {
    StoreMap incomingStores;
    const auto predIt = predecessors.find(block.name());
    if (predIt != predecessors.end() && predIt->second.size() == 1) {
      const auto& pred = predIt->second.front();
      const auto succCountIt = successorCounts.find(pred);
      const auto exitIt = exitStores.find(pred);
      if (succCountIt != successorCounts.end() && succCountIt->second == 1 &&
          exitIt != exitStores.end()) {
        incomingStores = exitIt->second;
      }
    }
    exitStores[block.name()] = rewriteBlock(block, definitions, std::move(incomingStores));
  }
}

}  // namespace

bool StoreToLoadForwardingPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runStoreToLoadForwarding(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
