#include "../../../include/compiler/backend/opt/CopyPropagation.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;

MachineOperand resolveAlias(const MachineOperand& operand,
                            const std::unordered_map<std::string, MachineOperand>& aliases) {
  if (!detail::isRegisterLike(operand)) {
    return operand;
  }

  MachineOperand current = operand;
  std::unordered_set<std::string> seen;
  while (detail::isRegisterLike(current)) {
    const auto key = detail::regKey(current);
    if (!seen.insert(key).second) {
      return operand;
    }
    const auto found = aliases.find(key);
    if (found == aliases.end()) {
      return current;
    }
    current = found->second;
  }
  return current;
}

void killAliasesMentioning(const std::string& key,
                           std::unordered_map<std::string, MachineOperand>& aliases) {
  aliases.erase(key);
  for (auto it = aliases.begin(); it != aliases.end();) {
    if (detail::isRegisterLike(it->second) && detail::regKey(it->second) == key) {
      it = aliases.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void CopyPropagation::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      std::unordered_map<std::string, MachineOperand> aliases;
      std::vector<MachineInstruction> rewritten;
      rewritten.reserve(block.instructions.size());

      for (auto instruction : block.instructions) {
        if (adapter.isCall(instruction)) {
          aliases.clear();
        }

        for (const auto& use : detail::usesOf(instruction, adapter)) {
          const auto replacement = resolveAlias(use, aliases);
          if (!detail::sameOperand(use, replacement)) {
            detail::replaceUseByKey(instruction, detail::regKey(use), replacement, adapter);
          }
        }

        for (const auto& def : detail::defsOf(instruction, adapter)) {
          killAliasesMentioning(detail::regKey(def), aliases);
        }

        if (instruction.kind == MachineInstructionKind::Copy && instruction.hasResult &&
            !instruction.operands.empty()) {
          const auto source = resolveAlias(instruction.operands[0], aliases);
          instruction.operands[0] = source;
          if (detail::sameOperand(instruction.result, source)) {
            continue;
          }
          if (detail::isRegisterLike(instruction.result)) {
            aliases[detail::regKey(instruction.result)] = source;
          }
        }

        rewritten.push_back(std::move(instruction));
      }

      block.instructions = std::move(rewritten);
    }
  }
}

}  // namespace compiler::backend::opt
