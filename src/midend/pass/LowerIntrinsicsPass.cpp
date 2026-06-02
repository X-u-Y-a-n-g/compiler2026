#include "../../../include/compiler/midend/pass/LowerIntrinsicsPass.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

std::string loweredCallee(const std::string& callee) {
  static const std::unordered_map<std::string, std::string> aliases = {
      {"@llvm.smax.i32", "@__builtin_smax_i32"},
      {"@llvm.smin.i32", "@__builtin_smin_i32"},
      {"@llvm.abs.i32", "@__builtin_abs_i32"},
  };
  const auto found = aliases.find(callee);
  return found == aliases.end() ? callee : found->second;
}

bool runLowerIntrinsics(Function& function) {
  bool changed = false;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "call" && !instruction.operands().empty()) {
        auto operands = instruction.operands();
        const auto lowered = loweredCallee(operands.front());
        if (lowered != operands.front()) {
          operands.front() = lowered;
          changed = true;
        }
        rewritten.emplace_back("call", std::move(operands), instruction.result());
        continue;
      }
      rewritten.push_back(instruction);
    }
    block.instructions() = std::move(rewritten);
  }
  return changed;
}

}  // namespace

bool LowerIntrinsicsPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runLowerIntrinsics(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
