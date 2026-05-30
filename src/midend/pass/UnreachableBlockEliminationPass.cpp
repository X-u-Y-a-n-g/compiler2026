#include "../../../include/compiler/midend/pass/UnreachableBlockEliminationPass.h"

#include <algorithm>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;

std::vector<std::string> successors(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto& operands = terminator->operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    return {operands[1], operands[2]};
  }
  return {};
}

void eliminateUnreachableBlocks(Function& function) {
  if (function.blocks().empty()) {
    return;
  }

  std::unordered_map<std::string, const BasicBlock*> blocksByName;
  for (const auto& block : function.blocks()) {
    blocksByName.emplace(block.name(), &block);
  }

  std::unordered_set<std::string> reachable;
  std::queue<std::string> worklist;
  worklist.push(function.blocks().front().name());

  while (!worklist.empty()) {
    const auto blockName = worklist.front();
    worklist.pop();
    if (!reachable.insert(blockName).second) {
      continue;
    }

    const auto found = blocksByName.find(blockName);
    if (found == blocksByName.end()) {
      continue;
    }
    for (const auto& successor : successors(*found->second)) {
      if (reachable.find(successor) == reachable.end()) {
        worklist.push(successor);
      }
    }
  }

  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock& block) {
                       return reachable.find(block.name()) == reachable.end();
                     }),
      function.blocks().end());
}

}  // namespace

void UnreachableBlockEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    eliminateUnreachableBlocks(function);
  }
}

}  // namespace compiler::midend::pass
