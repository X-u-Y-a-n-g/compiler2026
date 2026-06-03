#include "../../../include/compiler/backend/opt/MachineCSE.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

std::string expressionKey(const compiler::backend::mir::MachineInstruction& instruction) {
  std::string key = instruction.opcode;
  for (const auto& operand : instruction.operands) {
    key += "|";
    key += detail::operandKey(operand);
  }
  return key;
}

bool canCSE(const compiler::backend::mir::MachineInstruction& instruction,
            const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (!detail::isPureInstruction(instruction, adapter)) {
    return false;
  }
  if (instruction.kind != compiler::backend::mir::MachineInstructionKind::Target) {
    return false;
  }
  if (instruction.opcode == "div" || instruction.opcode == "sdiv" || instruction.opcode == "mod" ||
      instruction.opcode == "srem" || instruction.opcode == "fdiv" ||
      instruction.opcode.rfind("fcmp.", 0) == 0) {
    return false;
  }
  return !detail::opcodeIsMemoryAccess(instruction.opcode);
}

void killExpressionsUsing(const compiler::backend::mir::MachineOperand& def,
                          std::unordered_map<std::string, compiler::backend::mir::MachineOperand>&
                              expressions) {
  const auto defRegKey = detail::regKey(def);
  const auto defOperandKey = detail::operandKey(def);
  for (auto it = expressions.begin(); it != expressions.end();) {
    if ((detail::isRegisterLike(it->second) && detail::regKey(it->second) == defRegKey) ||
        it->first.find("|" + defOperandKey) != std::string::npos) {
      it = expressions.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void MachineCSE::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      std::unordered_map<std::string, compiler::backend::mir::MachineOperand> expressions;

      for (auto& instruction : block.instructions) {
        if (adapter.isCall(instruction) || instruction.opcode == "store") {
          expressions.clear();
        }

        for (const auto& def : detail::defsOf(instruction, adapter)) {
          killExpressionsUsing(def, expressions);
        }

        if (!canCSE(instruction, adapter)) {
          continue;
        }

        const auto key = expressionKey(instruction);
        const auto found = expressions.find(key);
        if (found != expressions.end()) {
          instruction = compiler::backend::mir::MachineInstruction::copy(instruction.result,
                                                                         found->second);
          continue;
        }

        expressions.emplace(key, instruction.result);
      }
    }
  }
}

}  // namespace compiler::backend::opt
