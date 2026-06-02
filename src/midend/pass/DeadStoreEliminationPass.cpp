#include "../../../include/compiler/midend/pass/DeadStoreEliminationPass.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"
#include "MemoryAccessUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

void rewriteBlock(BasicBlock& block,
                  const std::unordered_map<std::string, Instruction>& definitions) {
  std::unordered_map<std::string, std::size_t> pendingStores;
  std::unordered_set<std::size_t> removed;
  std::vector<Instruction> kept;
  kept.reserve(block.instructions().size());

  for (const auto& instruction : block.instructions()) {
    if (memory::isLoad(instruction)) {
      const auto address = memory::loadAddress(instruction, definitions);
      if (address.kind == memory::AddressClass::Exact) {
        pendingStores.erase(address.key);
      } else if (address.kind == memory::AddressClass::Derived) {
        pendingStores.clear();
      } else {
        pendingStores.clear();
      }
      kept.push_back(instruction);
      continue;
    }

    if (memory::isStore(instruction)) {
      const auto address = memory::storeAddress(instruction, definitions);
      if (address.kind == memory::AddressClass::Exact) {
        const auto previous = pendingStores.find(address.key);
        if (previous != pendingStores.end()) {
          removed.insert(previous->second);
        }
        pendingStores[address.key] = kept.size();
      } else if (address.kind == memory::AddressClass::Derived) {
        const auto previous = pendingStores.find(address.key);
        if (previous != pendingStores.end()) {
          removed.insert(previous->second);
        }
        pendingStores.clear();
        pendingStores[address.key] = kept.size();
      } else {
        pendingStores.clear();
      }
      kept.push_back(instruction);
      continue;
    }

    if (instruction.opcode() == "call") {
      pendingStores.clear();
    }
    kept.push_back(instruction);
  }

  if (removed.empty()) {
    return;
  }

  std::vector<Instruction> rewritten;
  rewritten.reserve(kept.size() - removed.size());
  for (std::size_t i = 0; i < kept.size(); ++i) {
    if (removed.find(i) == removed.end()) {
      rewritten.push_back(std::move(kept[i]));
    }
  }
  block.instructions() = std::move(rewritten);
}

void runDeadStoreElimination(Function& function) {
  const auto definitions = memory::collectDefinitions(function);
  for (auto& block : function.blocks()) {
    rewriteBlock(block, definitions);
  }
}

}  // namespace

bool DeadStoreEliminationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runDeadStoreElimination(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
