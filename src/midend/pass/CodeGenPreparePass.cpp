#include "../../../include/compiler/midend/pass/CodeGenPreparePass.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

void rewriteFunctionOperands(
    Function& function, const std::unordered_map<std::string, std::string>& aliases) {
  if (aliases.empty()) {
    return;
  }
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(), aliases),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

bool runCodeGenPrepare(Function& function) {
  bool changed = false;
  std::unordered_map<std::string, std::string> aliases;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);

      if (instruction.opcode() == "neg" && instruction.hasResult() && operands.size() == 1) {
        rewritten.emplace_back("sub", std::vector<std::string>{"0", operands[0]},
                               instruction.result());
        changed = true;
        continue;
      }
      if (instruction.opcode() == "div" && instruction.hasResult()) {
        rewritten.emplace_back("sdiv", std::move(operands), instruction.result());
        changed = true;
        continue;
      }
      if (instruction.opcode() == "mod" && instruction.hasResult()) {
        rewritten.emplace_back("srem", std::move(operands), instruction.result());
        changed = true;
        continue;
      }
      if (instruction.hasResult()) {
        aliases.erase(instruction.result());
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
  rewriteFunctionOperands(function, aliases);
  return changed;
}

}  // namespace

bool CodeGenPreparePass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runCodeGenPrepare(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
