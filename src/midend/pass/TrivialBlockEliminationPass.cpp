#include "../../../include/compiler/midend/pass/TrivialBlockEliminationPass.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto& operands = terminator->operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

std::unordered_map<std::string, std::vector<std::string>> computePredecessors(
    const Function& function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto& block : function.blocks()) {
    for (const auto& target : branchTargets(block)) {
      predecessors[target].push_back(block.name());
    }
  }
  return predecessors;
}

const BasicBlock* findBlock(const Function& function, const std::string& name) {
  for (const auto& block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

bool blockStartsWithPhi(const Function& function, const std::string& name) {
  const auto* block = findBlock(function, name);
  return block != nullptr && !block->instructions().empty() &&
         block->instructions().front().opcode() == "phi";
}

bool phiConflictAfterRedirect(
    const Function& function, const std::string& blockName, const std::string& targetName,
    const std::unordered_map<std::string, std::vector<std::string>>& predecessors) {
  const auto* target = findBlock(function, targetName);
  if (target == nullptr) {
    return false;
  }

  const auto predIt = predecessors.find(blockName);
  if (predIt == predecessors.end()) {
    return false;
  }

  for (const auto& instruction : target->instructions()) {
    if (instruction.opcode() != "phi") {
      break;
    }
    const auto& operands = instruction.operands();
    for (std::size_t i = 2; i < operands.size(); i += 2) {
      if (operands[i] != blockName) {
        continue;
      }
      const auto& redirectedValue = operands[i - 1];
      for (const auto& pred : predIt->second) {
        for (std::size_t j = 2; j < operands.size(); j += 2) {
          if (operands[j] == pred && operands[j - 1] != redirectedValue) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

void expandIncomingBlock(const std::string& block,
                         const std::unordered_map<std::string, std::string>& redirects,
                         const std::unordered_map<std::string, std::vector<std::string>>& predecessors,
                         std::vector<std::string>& out) {
  if (redirects.find(block) == redirects.end()) {
    out.push_back(block);
    return;
  }

  const auto foundPreds = predecessors.find(block);
  if (foundPreds == predecessors.end()) {
    out.push_back(resolveRedirect(block, redirects));
    return;
  }
  for (const auto& pred : foundPreds->second) {
    expandIncomingBlock(pred, redirects, predecessors, out);
  }
}

void rewriteBranchTargets(Function& function,
                          const std::unordered_map<std::string, std::string>& redirects,
                          const std::unordered_map<std::string, std::vector<std::string>>& predecessors) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      if (instruction.opcode() == "br") {
        if (operands.size() == 1) {
          operands[0] = resolveRedirect(operands[0], redirects);
        } else if (operands.size() == 3) {
          operands[1] = resolveRedirect(operands[1], redirects);
          operands[2] = resolveRedirect(operands[2], redirects);
        }
        instruction = Instruction{"br", std::move(operands), instruction.result()};
        continue;
      }

      if (instruction.opcode() == "phi") {
        std::vector<std::string> rewritten;
        if (!operands.empty()) {
          rewritten.push_back(operands.front());
        }
        for (std::size_t i = 2; i < operands.size(); i += 2) {
          std::vector<std::string> incomingBlocks;
          expandIncomingBlock(operands[i], redirects, predecessors, incomingBlocks);
          for (const auto& incomingBlock : incomingBlocks) {
            rewritten.push_back(operands[i - 1]);
            rewritten.push_back(incomingBlock);
          }
        }
        instruction = Instruction{"phi", std::move(rewritten), instruction.result()};
      }
    }
  }
}

void eliminateTrivialBlocks(Function& function) {
  if (function.blocks().empty()) {
    return;
  }

  const auto predecessors = computePredecessors(function);
  const auto& entryName = function.blocks().front().name();
  std::unordered_map<std::string, std::string> redirects;
  for (const auto& block : function.blocks()) {
    if (block.name() == entryName || !isTrivialJumpBlock(block)) {
      continue;
    }
    const auto& target = block.instructions().front().operands().front();
    if (target != block.name() &&
        !blockStartsWithPhi(function, target) &&
        !phiConflictAfterRedirect(function, block.name(), target, predecessors)) {
      redirects.emplace(block.name(), target);
    }
  }

  if (redirects.empty()) {
    return;
  }

  for (auto& [from, to] : redirects) {
    to = resolveRedirect(to, redirects);
  }
  rewriteBranchTargets(function, redirects, predecessors);

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
