#include "../../../include/compiler/backend/opt/PostRAScheduler.h"

#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

constexpr std::size_t kMaxInstructionsForScheduling = 8192;

bool independent(const compiler::backend::mir::MachineInstruction& first,
                 const compiler::backend::mir::MachineInstruction& second,
                 const compiler::backend::target::TargetInstrAdapter& adapter) {
  for (const auto& def : detail::defsOf(first, adapter)) {
    const auto defKey = detail::regKey(def);
    for (const auto& use : detail::usesOf(second, adapter)) {
      if (defKey == detail::regKey(use)) {
        return false;
      }
    }
    for (const auto& laterDef : detail::defsOf(second, adapter)) {
      if (defKey == detail::regKey(laterDef)) {
        return false;
      }
    }
  }
  for (const auto& use : detail::usesOf(first, adapter)) {
    const auto useKey = detail::regKey(use);
    for (const auto& def : detail::defsOf(second, adapter)) {
      if (useKey == detail::regKey(def)) {
        return false;
      }
    }
  }
  return true;
}

bool movableAfterRA(const compiler::backend::mir::MachineInstruction& instruction,
                    const compiler::backend::target::TargetInstrAdapter& adapter) {
  return detail::isPureInstruction(instruction, adapter) &&
         instruction.kind != compiler::backend::mir::MachineInstructionKind::Phi;
}

}  // namespace

void PostRAScheduler::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      if (block.instructions.size() > kMaxInstructionsForScheduling) {
        continue;
      }
      for (std::size_t i = 1; i < block.instructions.size(); ++i) {
        auto& previous = block.instructions[i - 1];
        auto& current = block.instructions[i];
        if (!movableAfterRA(current, adapter) || !movableAfterRA(previous, adapter)) {
          continue;
        }
        const auto currentUses = detail::usesOf(current, adapter).size();
        const auto previousUses = detail::usesOf(previous, adapter).size();
        if (currentUses < previousUses && independent(previous, current, adapter)) {
          std::swap(previous, current);
        }
      }
    }
  }
}

}  // namespace compiler::backend::opt
