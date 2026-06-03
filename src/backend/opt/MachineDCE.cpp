#include "../../../include/compiler/backend/opt/MachineDCE.h"

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/common/CFGBuilder.h"
#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

void MachineDCE::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    const auto cfg = compiler::backend::common::CFGBuilder{}.build(function, adapter);

    std::vector<std::unordered_set<std::string>> useSets(function.blocks.size());
    std::vector<std::unordered_set<std::string>> defSets(function.blocks.size());
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
      std::unordered_set<std::string> seenDefs;
      for (const auto& instruction : function.blocks[blockIndex].instructions) {
        for (const auto& use : detail::usesOf(instruction, adapter)) {
          const auto key = detail::regKey(use);
          if (!key.empty() && seenDefs.find(key) == seenDefs.end()) {
            useSets[blockIndex].insert(key);
          }
        }
        for (const auto& def : detail::defsOf(instruction, adapter)) {
          const auto key = detail::regKey(def);
          if (!key.empty()) {
            defSets[blockIndex].insert(key);
            seenDefs.insert(key);
          }
        }
      }
    }

    std::vector<std::unordered_set<std::string>> liveIn(function.blocks.size());
    std::vector<std::unordered_set<std::string>> liveOut(function.blocks.size());
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t reverse = function.blocks.size(); reverse > 0; --reverse) {
        const auto blockIndex = reverse - 1;
        std::unordered_set<std::string> newOut;
        for (const auto succ : cfg.successors[blockIndex]) {
          newOut.insert(liveIn[succ].begin(), liveIn[succ].end());
        }

        auto newIn = useSets[blockIndex];
        for (const auto& live : newOut) {
          if (defSets[blockIndex].find(live) == defSets[blockIndex].end()) {
            newIn.insert(live);
          }
        }

        if (newOut != liveOut[blockIndex] || newIn != liveIn[blockIndex]) {
          liveOut[blockIndex] = std::move(newOut);
          liveIn[blockIndex] = std::move(newIn);
          changed = true;
        }
      }
    }

    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
      auto& block = function.blocks[blockIndex];
      auto live = liveOut[blockIndex];
      std::vector<bool> removeInstruction(block.instructions.size(), false);

      for (std::size_t reverse = block.instructions.size(); reverse > 0; --reverse) {
        const auto index = reverse - 1;
        const auto& instruction = block.instructions[index];
        const auto defs = detail::defsOf(instruction, adapter);

        bool anyDefLive = false;
        for (const auto& def : defs) {
          if (live.find(detail::regKey(def)) != live.end()) {
            anyDefLive = true;
          }
        }

        const bool removable = detail::isPureInstruction(instruction, adapter) && !defs.empty();
        if (removable && !anyDefLive) {
          removeInstruction[index] = true;
        } else {
          for (const auto& def : defs) {
            live.erase(detail::regKey(def));
          }
          for (const auto& use : detail::usesOf(instruction, adapter)) {
            const auto key = detail::regKey(use);
            if (!key.empty()) {
              live.insert(key);
            }
          }
        }
      }

      detail::removeDeadInstructionsByMask(block, removeInstruction);
    }
  }
}

}  // namespace compiler::backend::opt
