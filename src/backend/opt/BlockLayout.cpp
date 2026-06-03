#include "../../../include/compiler/backend/opt/BlockLayout.h"

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

bool hasExplicitTerminator(
    const compiler::backend::mir::MachineBlock& block,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (block.instructions.empty()) {
    return false;
  }
  const auto& terminator = block.instructions.back();
  return adapter.isReturn(terminator) || adapter.isConditionalBranch(terminator) ||
         adapter.isUnconditionalBranch(terminator);
}

}  // namespace

void BlockLayout::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    if (function.blocks.size() < 2) {
      continue;
    }

    bool safeToReorder = true;
    for (const auto& block : function.blocks) {
      if (!hasExplicitTerminator(block, adapter)) {
        safeToReorder = false;
        break;
      }
    }
    if (!safeToReorder) {
      continue;
    }

    const auto reachable = detail::reachableBlocks(function, adapter);
    std::unordered_set<std::size_t> placed(reachable.begin(), reachable.end());
    std::vector<compiler::backend::mir::MachineBlock> laidOut;
    laidOut.reserve(function.blocks.size());
    for (const auto index : reachable) {
      laidOut.push_back(std::move(function.blocks[index]));
    }
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
      if (placed.find(i) == placed.end()) {
        laidOut.push_back(std::move(function.blocks[i]));
      }
    }
    function.blocks = std::move(laidOut);
  }
}

}  // namespace compiler::backend::opt
