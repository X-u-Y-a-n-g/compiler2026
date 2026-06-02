#include "../../../include/compiler/midend/pass/GlobalConstPropagationPass.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

std::string globalOperand(const std::string& name) { return "@" + name; }

bool isScalarObjectType(const std::string& type) {
  return type.find('[') == std::string::npos;
}

std::unordered_map<std::string, std::string> collectScalarConstGlobals(const Module& module) {
  std::unordered_map<std::string, std::string> constants;
  for (const auto& global : module.globals()) {
    if (!global.isConst() || !global.initializer().isScalar() ||
        !isScalarObjectType(global.type())) {
      continue;
    }
    constants.emplace(globalOperand(global.name()), global.initializer().literal());
  }
  return constants;
}

std::unordered_set<std::string> unsafeGlobalAddressUses(
    const Module& module, const std::unordered_map<std::string, std::string>& constants) {
  std::unordered_set<std::string> unsafe;
  for (const auto& function : module.functions()) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        const auto& operands = instruction.operands();
        for (std::size_t i = 0; i < operands.size(); ++i) {
          if (constants.find(operands[i]) == constants.end()) {
            continue;
          }
          const bool safeLoadAddress =
              instruction.opcode() == "load" && operands.size() == 2 && i == 1;
          if (!safeLoadAddress) {
            unsafe.insert(operands[i]);
          }
        }
      }
    }
  }
  return unsafe;
}

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

bool runGlobalConstProp(Module& module) {
  const auto constants = collectScalarConstGlobals(module);
  if (constants.empty()) {
    return false;
  }
  const auto unsafe = unsafeGlobalAddressUses(module, constants);

  bool changed = false;
  for (auto& function : module.functions()) {
    std::unordered_map<std::string, std::string> aliases;
    for (auto& block : function.blocks()) {
      std::vector<Instruction> rewritten;
      rewritten.reserve(block.instructions().size());
      for (const auto& instruction : block.instructions()) {
        auto operands = detail::rewriteOperands(instruction.operands(), aliases);
        if (instruction.opcode() == "load" && instruction.hasResult() && operands.size() == 2) {
          const auto constant = constants.find(operands[1]);
          if (constant != constants.end() && unsafe.find(operands[1]) == unsafe.end()) {
            aliases[instruction.result()] = constant->second;
            changed = true;
            continue;
          }
        }
        if (instruction.hasResult()) {
          aliases.erase(instruction.result());
        }
        rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
      }
      block.instructions() = std::move(rewritten);
    }
    rewriteFunctionOperands(function, aliases);
  }
  return changed;
}

}  // namespace

bool GlobalConstPropagationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  runGlobalConstProp(module);
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
