#include "../../../include/compiler/midend/pass/TrivialBlockEliminationPass.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

bool isTrivialJumpBlock(const BasicBlock &block) {
  return block.instructions().size() == 1 &&
         block.instructions().front().opcode() == "br" &&
         block.instructions().front().operands().size() == 1;
}

bool isTrueConstant(const std::string &operand) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value != 0;
}

bool isFalseConstant(const std::string &operand) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value == 0;
}

std::string
resolveRedirect(const std::string &target,
                const std::unordered_map<std::string, std::string> &redirects) {
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

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto &operands = terminator->operands();
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

std::unordered_map<std::string, std::vector<std::string>>
computePredecessors(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto &block : function.blocks()) {
    for (const auto &target : branchTargets(block)) {
      predecessors[target].push_back(block.name());
    }
  }
  return predecessors;
}

const BasicBlock *findBlock(const Function &function, const std::string &name) {
  for (const auto &block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

std::unordered_set<std::string> reachableBlocks(const Function &function) {
  std::unordered_set<std::string> reachable;
  if (function.blocks().empty()) {
    return reachable;
  }

  std::unordered_map<std::string, const BasicBlock *> blockByName;
  for (const auto &block : function.blocks()) {
    blockByName.emplace(block.name(), &block);
  }

  std::deque<std::string> worklist;
  reachable.insert(function.blocks().front().name());
  worklist.push_back(function.blocks().front().name());
  while (!worklist.empty()) {
    const auto blockName = worklist.front();
    worklist.pop_front();
    const auto found = blockByName.find(blockName);
    if (found == blockByName.end()) {
      continue;
    }
    for (const auto &target : branchTargets(*found->second)) {
      if (blockByName.find(target) != blockByName.end() &&
          reachable.insert(target).second) {
        worklist.push_back(target);
      }
    }
  }
  return reachable;
}

bool simplifyConstantBranches(Function &function) {
  bool changed = false;
  for (auto &block : function.blocks()) {
    auto &instructions = block.instructions();
    if (instructions.empty()) {
      continue;
    }

    const auto &terminator = instructions.back();
    if (terminator.opcode() != "br" || terminator.operands().size() != 3) {
      continue;
    }

    const auto &operands = terminator.operands();
    std::string target;
    if (operands[1] == operands[2]) {
      target = operands[1];
    } else if (isTrueConstant(operands[0])) {
      target = operands[1];
    } else if (isFalseConstant(operands[0])) {
      target = operands[2];
    } else {
      continue;
    }

    instructions.back() = Instruction{"br", {std::move(target)}};
    changed = true;
  }
  return changed;
}

bool removeUnreachableBlocks(Function &function) {
  const auto reachable = reachableBlocks(function);
  const auto oldSize = function.blocks().size();
  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock &block) {
                       return reachable.find(block.name()) == reachable.end();
                     }),
      function.blocks().end());
  return oldSize != function.blocks().size();
}

bool prunePhiIncoming(Function &function) {
  const auto predecessors = computePredecessors(function);
  bool changed = false;

  for (auto &block : function.blocks()) {
    const auto predIt = predecessors.find(block.name());
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }

      std::vector<std::string> operands;
      const auto &oldOperands = instruction.operands();
      if (!oldOperands.empty()) {
        operands.push_back(oldOperands.front());
      }
      for (std::size_t i = 1; i + 1 < oldOperands.size(); i += 2) {
        const auto &value = oldOperands[i];
        const auto &pred = oldOperands[i + 1];
        if (predIt != predecessors.end() &&
            predIt->second.end() !=
                std::find(predIt->second.begin(), predIt->second.end(), pred)) {
          operands.push_back(value);
          operands.push_back(pred);
        }
      }

      if (operands.size() != oldOperands.size()) {
        changed = true;
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  return changed;
}

bool phiConflictAfterRedirect(
    const Function &function, const std::string &blockName,
    const std::string &targetName,
    const std::unordered_map<std::string, std::vector<std::string>>
        &predecessors) {
  const auto *target = findBlock(function, targetName);
  if (target == nullptr) {
    return false;
  }

  const auto predIt = predecessors.find(blockName);
  if (predIt == predecessors.end()) {
    return false;
  }

  for (const auto &instruction : target->instructions()) {
    if (instruction.opcode() != "phi") {
      break;
    }
    const auto &operands = instruction.operands();
    for (std::size_t i = 2; i < operands.size(); i += 2) {
      if (operands[i] != blockName) {
        continue;
      }
      const auto &redirectedValue = operands[i - 1];
      for (const auto &pred : predIt->second) {
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

bool phiConflictWithPlannedRedirects(
    const Function &function, const std::string &blockName,
    const std::string &targetName,
    const std::unordered_map<std::string, std::vector<std::string>>
        &predecessors,
    const std::unordered_map<std::string, std::string> &plannedPhiValues) {
  const auto *target = findBlock(function, targetName);
  if (target == nullptr) {
    return false;
  }
  const auto predIt = predecessors.find(blockName);
  if (predIt == predecessors.end()) {
    return false;
  }

  for (const auto &instruction : target->instructions()) {
    if (instruction.opcode() != "phi") {
      break;
    }
    const auto &operands = instruction.operands();
    for (std::size_t i = 2; i < operands.size(); i += 2) {
      if (operands[i] != blockName) {
        continue;
      }
      for (const auto &pred : predIt->second) {
        const std::string key =
            targetName + "\n" + instruction.result() + "\n" + pred;
        const auto planned = plannedPhiValues.find(key);
        if (planned != plannedPhiValues.end() &&
            planned->second != operands[i - 1]) {
          return true;
        }
      }
    }
  }
  return false;
}

void recordPlannedPhiRedirect(
    const Function &function, const std::string &blockName,
    const std::string &targetName,
    const std::unordered_map<std::string, std::vector<std::string>>
        &predecessors,
    std::unordered_map<std::string, std::string> &plannedPhiValues) {
  const auto *target = findBlock(function, targetName);
  if (target == nullptr) {
    return;
  }
  const auto predIt = predecessors.find(blockName);
  if (predIt == predecessors.end()) {
    return;
  }

  for (const auto &instruction : target->instructions()) {
    if (instruction.opcode() != "phi") {
      break;
    }
    const auto &operands = instruction.operands();
    for (std::size_t i = 2; i < operands.size(); i += 2) {
      if (operands[i] != blockName) {
        continue;
      }
      for (const auto &pred : predIt->second) {
        const std::string key =
            targetName + "\n" + instruction.result() + "\n" + pred;
        plannedPhiValues.emplace(key, operands[i - 1]);
      }
    }
  }
}

void expandIncomingBlock(
    const std::string &block,
    const std::unordered_map<std::string, std::string> &redirects,
    const std::unordered_map<std::string, std::vector<std::string>>
        &predecessors,
    std::vector<std::string> &out) {
  if (redirects.find(block) == redirects.end()) {
    out.push_back(block);
    return;
  }

  const auto foundPreds = predecessors.find(block);
  if (foundPreds == predecessors.end()) {
    out.push_back(resolveRedirect(block, redirects));
    return;
  }
  for (const auto &pred : foundPreds->second) {
    expandIncomingBlock(pred, redirects, predecessors, out);
  }
}

void rewriteBranchTargets(
    Function &function,
    const std::unordered_map<std::string, std::string> &redirects,
    const std::unordered_map<std::string, std::vector<std::string>>
        &predecessors) {
  for (auto &block : function.blocks()) {
    for (auto &instruction : block.instructions()) {
      auto operands = instruction.operands();
      if (instruction.opcode() == "br") {
        if (operands.size() == 1) {
          operands[0] = resolveRedirect(operands[0], redirects);
        } else if (operands.size() == 3) {
          operands[1] = resolveRedirect(operands[1], redirects);
          operands[2] = resolveRedirect(operands[2], redirects);
        }
        instruction =
            Instruction{"br", std::move(operands), instruction.result()};
        continue;
      }

      if (instruction.opcode() == "phi") {
        std::vector<std::string> rewritten;
        if (!operands.empty()) {
          rewritten.push_back(operands.front());
        }
        for (std::size_t i = 2; i < operands.size(); i += 2) {
          std::vector<std::string> incomingBlocks;
          expandIncomingBlock(operands[i], redirects, predecessors,
                              incomingBlocks);
          for (const auto &incomingBlock : incomingBlocks) {
            rewritten.push_back(operands[i - 1]);
            rewritten.push_back(incomingBlock);
          }
        }
        instruction =
            Instruction{"phi", std::move(rewritten), instruction.result()};
      }
    }
  }
}

void rewriteFunctionOperands(
    Function &function,
    const std::unordered_map<std::string, std::string> &aliases) {
  if (aliases.empty()) {
    return;
  }

  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      rewritten.emplace_back(
          instruction.opcode(),
          detail::rewriteOperands(instruction.operands(), aliases),
          instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void replacePhiIncomingBlock(Function &function, const std::string &oldName,
                             const std::string &newName) {
  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }

      auto operands = instruction.operands();
      for (std::size_t i = 2; i < operands.size(); i += 2) {
        if (operands[i] == oldName) {
          operands[i] = newName;
        }
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

bool canMergeLinearSuccessor(const BasicBlock &block,
                             const BasicBlock &successor) {
  bool sawNonPhi = false;
  for (const auto &instruction : successor.instructions()) {
    if (instruction.opcode() == "phi") {
      if (sawNonPhi || !instruction.hasResult()) {
        return false;
      }
      const auto &operands = instruction.operands();
      bool hasIncoming = false;
      for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
        if (operands[i + 1] == block.name()) {
          hasIncoming = true;
          break;
        }
      }
      if (!hasIncoming) {
        return false;
      }
      continue;
    }
    sawNonPhi = true;
  }
  return true;
}

bool mergeLinearBlocks(Function &function) {
  if (function.blocks().empty()) {
    return false;
  }

  const auto predecessors = computePredecessors(function);
  const auto entryName = function.blocks().front().name();
  for (auto blockIt = function.blocks().begin();
       blockIt != function.blocks().end(); ++blockIt) {
    auto &instructions = blockIt->instructions();
    if (instructions.empty() || instructions.back().opcode() != "br" ||
        instructions.back().operands().size() != 1) {
      continue;
    }

    const auto successorName = instructions.back().operands().front();
    if (successorName == blockIt->name() || successorName == entryName) {
      continue;
    }
    const auto predIt = predecessors.find(successorName);
    if (predIt == predecessors.end() || predIt->second.size() != 1 ||
        predIt->second.front() != blockIt->name()) {
      continue;
    }

    auto successorIt =
        std::find_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock &candidate) {
                       return candidate.name() == successorName;
                     });
    if (successorIt == function.blocks().end() ||
        !canMergeLinearSuccessor(*blockIt, *successorIt)) {
      continue;
    }

    std::unordered_map<std::string, std::string> aliases;
    std::vector<Instruction> merged = instructions;
    merged.pop_back();
    for (const auto &instruction : successorIt->instructions()) {
      if (instruction.opcode() == "phi") {
        const auto &operands = instruction.operands();
        for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
          if (operands[i + 1] == blockIt->name()) {
            aliases[instruction.result()] =
                detail::resolveAlias(operands[i], aliases);
            break;
          }
        }
        continue;
      }
      merged.emplace_back(
          instruction.opcode(),
          detail::rewriteOperands(instruction.operands(), aliases),
          instruction.result());
    }

    blockIt->instructions() = std::move(merged);
    const auto oldSuccessorName = successorIt->name();
    const auto newBlockName = blockIt->name();
    function.blocks().erase(successorIt);
    replacePhiIncomingBlock(function, oldSuccessorName, newBlockName);
    rewriteFunctionOperands(function, aliases);
    return true;
  }

  return false;
}

bool eliminateTrivialBlocks(Function &function) {
  if (function.blocks().empty()) {
    return false;
  }

  const auto predecessors = computePredecessors(function);
  const auto &entryName = function.blocks().front().name();
  std::unordered_map<std::string, std::string> redirects;
  std::unordered_map<std::string, std::string> plannedPhiValues;
  for (const auto &block : function.blocks()) {
    if (block.name() == entryName || !isTrivialJumpBlock(block)) {
      continue;
    }
    const auto &target = block.instructions().front().operands().front();
    if (target != block.name() &&
        !phiConflictAfterRedirect(function, block.name(), target,
                                  predecessors) &&
        !phiConflictWithPlannedRedirects(function, block.name(), target,
                                         predecessors, plannedPhiValues)) {
      redirects.emplace(block.name(), target);
      recordPlannedPhiRedirect(function, block.name(), target, predecessors,
                               plannedPhiValues);
    }
  }

  if (redirects.empty()) {
    return false;
  }

  for (auto &[from, to] : redirects) {
    to = resolveRedirect(to, redirects);
  }
  rewriteBranchTargets(function, redirects, predecessors);

  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock &block) {
                       return redirects.find(block.name()) != redirects.end();
                     }),
      function.blocks().end());
  return true;
}

void simplifyCfg(Function &function) {
  bool changed = false;
  do {
    changed = false;
    changed |= simplifyConstantBranches(function);
    changed |= removeUnreachableBlocks(function);
    changed |= prunePhiIncoming(function);
    changed |= mergeLinearBlocks(function);
    changed |= eliminateTrivialBlocks(function);
    changed |= prunePhiIncoming(function);
  } while (changed);
}

} // namespace

bool TrivialBlockEliminationPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    simplifyCfg(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
