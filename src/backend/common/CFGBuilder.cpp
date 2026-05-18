#include "../../../include/compiler/backend/common/CFGBuilder.h"

#include <unordered_set>

namespace compiler::backend::common {

ControlFlowGraph CFGBuilder::build(
    const compiler::backend::mir::MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) const {
  ControlFlowGraph cfg;
  cfg.function = &function;
  cfg.blocks.reserve(function.blocks.size());
  cfg.successors.resize(function.blocks.size());
  cfg.predecessors.resize(function.blocks.size());

  for (std::size_t i = 0; i < function.blocks.size(); ++i) {
    cfg.blocks.push_back(&function.blocks[i]);
    cfg.indexByName.emplace(function.blocks[i].name, i);
  }

  for (std::size_t i = 0; i < function.blocks.size(); ++i) {
    const auto& block = function.blocks[i];
    std::unordered_set<std::size_t> succs;
    if (!block.instructions.empty()) {
      const auto& terminator = block.instructions.back();
      if (adapter.isConditionalBranch(terminator) || adapter.isUnconditionalBranch(terminator)) {
        for (const auto& target : adapter.branchTargets(terminator)) {
          const auto found = cfg.indexByName.find(target);
          if (found != cfg.indexByName.end()) {
            succs.insert(found->second);
          }
        }
      } else if (!adapter.isReturn(terminator) && i + 1 < function.blocks.size()) {
        succs.insert(i + 1);
      }
    } else if (i + 1 < function.blocks.size()) {
      succs.insert(i + 1);
    }

    cfg.successors[i].assign(succs.begin(), succs.end());
    for (const auto succ : cfg.successors[i]) {
      cfg.predecessors[succ].push_back(i);
    }
  }

  return cfg;
}

}  // namespace compiler::backend::common
