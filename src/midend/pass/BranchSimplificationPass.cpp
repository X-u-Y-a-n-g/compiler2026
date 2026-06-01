#include "../../../include/compiler/midend/pass/BranchSimplificationPass.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

std::unordered_map<std::string, const BasicBlock*> indexBlocks(const Function& function) {
  std::unordered_map<std::string, const BasicBlock*> blocks;
  for (const auto& block : function.blocks()) {
    blocks.emplace(block.name(), &block);
  }
  return blocks;
}

bool isTrivialJumpBlock(const BasicBlock& block) {
  return block.instructions().size() == 1 && block.instructions().front().opcode() == "br" &&
         block.instructions().front().operands().size() == 1;
}

bool blockStartsWithPhi(const std::unordered_map<std::string, const BasicBlock*>& blocks,
                        const std::string& name) {
  const auto found = blocks.find(name);
  return found != blocks.end() && !found->second->instructions().empty() &&
         found->second->instructions().front().opcode() == "phi";
}

std::string resolveJumpTarget(const std::unordered_map<std::string, const BasicBlock*>& blocks,
                              const std::string& target) {
  std::string resolved = target;
  std::unordered_set<std::string> seen;

  while (seen.insert(resolved).second) {
    const auto found = blocks.find(resolved);
    if (found == blocks.end() || !isTrivialJumpBlock(*found->second)) {
      break;
    }

    const std::string next = found->second->instructions().front().operands().front();
    if (next == resolved || blockStartsWithPhi(blocks, next)) {
      break;
    }
    resolved = next;
  }

  return resolved;
}

Instruction simplifyBranch(const Instruction& instruction,
                           const std::unordered_map<std::string, const BasicBlock*>& blocks) {
  if (instruction.opcode() != "br") {
    return instruction;
  }

  auto operands = instruction.operands();
  if (operands.size() == 1) {
    operands[0] = resolveJumpTarget(blocks, operands[0]);
    return Instruction{"br", std::move(operands)};
  }

  if (operands.size() != 3) {
    return instruction;
  }

  operands[1] = resolveJumpTarget(blocks, operands[1]);
  operands[2] = resolveJumpTarget(blocks, operands[2]);

  long long condition = 0;
  if (detail::parseIntegerLiteral(operands[0], condition)) {
    return Instruction{"br", {condition != 0 ? operands[1] : operands[2]}};
  }
  if (operands[1] == operands[2]) {
    return Instruction{"br", {operands[1]}};
  }

  return Instruction{"br", std::move(operands)};
}

void simplifyFunction(Function& function) {
  const auto blocks = indexBlocks(function);
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    bool terminated = false;
    for (const auto& instruction : block.instructions()) {
      if (terminated) {
        continue;
      }

      Instruction current = simplifyBranch(instruction, blocks);
      terminated = current.isTerminator();
      rewritten.push_back(std::move(current));
    }

    block.instructions() = std::move(rewritten);
  }
}

}  // namespace

void BranchSimplificationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    simplifyFunction(function);
  }
}

}  // namespace compiler::midend::pass
