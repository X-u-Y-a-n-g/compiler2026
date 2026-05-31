#include "../../../include/compiler/midend/pass/TrivialBlockEliminationPass.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

bool isTrivialJumpBlock(const BasicBlock& block) {
  return block.instructions().size() == 1 && block.instructions().front().opcode() == "br" &&
         block.instructions().front().operands().size() == 1;
}

std::string resolveRedirect(const std::string& target,
                            const std::unordered_map<std::string, std::string>& redirects) {
  std::string resolved = target;
  std::unordered_set<std::string> seen;
  while (true) {
    const auto found = redirects.find(resolved);
    if (found == redirects.end() || !seen.insert(resolved).second) {
      return resolved;
    }
    resolved = found->second;
  }
}

void rewriteBranchTargets(Function& function,
                          const std::unordered_map<std::string, std::string>& redirects) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "br") {
        continue;
      }

      auto operands = instruction.operands();
      if (operands.size() == 1) {
        operands[0] = resolveRedirect(operands[0], redirects);
      } else if (operands.size() == 3) {
        operands[1] = resolveRedirect(operands[1], redirects);
        operands[2] = resolveRedirect(operands[2], redirects);
      }
      instruction = Instruction{"br", std::move(operands), instruction.result()};
    }
  }
}

void eliminateTrivialBlocks(Function& function) {
  if (function.blocks().empty()) {
    return;
  }

  const auto& entryName = function.blocks().front().name();
  std::unordered_map<std::string, std::string> redirects;
  for (const auto& block : function.blocks()) {
    if (block.name() == entryName || !isTrivialJumpBlock(block)) {
      continue;
    }
    const auto& target = block.instructions().front().operands().front();
    if (target != block.name()) {
      redirects.emplace(block.name(), target);
    }
  }

  if (redirects.empty()) {
    return;
  }

  for (auto& [from, to] : redirects) {
    to = resolveRedirect(to, redirects);
  }
  rewriteBranchTargets(function, redirects);

  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock& block) {
                       return redirects.find(block.name()) != redirects.end();
                     }),
      function.blocks().end());
}

}  // namespace

void TrivialBlockEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    eliminateTrivialBlocks(function);
  }
}

}  // namespace compiler::midend::pass
