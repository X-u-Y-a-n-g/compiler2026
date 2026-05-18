#pragma once

#include <string>
#include <vector>

#include "MachineModule.h"

namespace compiler::backend::target {

class TargetInstrAdapter {
 public:
  virtual ~TargetInstrAdapter() = default;

  [[nodiscard]] virtual bool isCall(const compiler::backend::mir::MachineInstruction& instruction) const = 0;
  [[nodiscard]] virtual bool isReturn(
      const compiler::backend::mir::MachineInstruction& instruction) const = 0;
  [[nodiscard]] virtual bool isUnconditionalBranch(
      const compiler::backend::mir::MachineInstruction& instruction) const = 0;
  [[nodiscard]] virtual bool isConditionalBranch(
      const compiler::backend::mir::MachineInstruction& instruction) const = 0;

  [[nodiscard]] virtual std::vector<std::string> branchTargets(
      const compiler::backend::mir::MachineInstruction& instruction) const = 0;

  virtual void enumerateUses(const compiler::backend::mir::MachineInstruction& instruction,
                             std::vector<compiler::backend::mir::MachineOperand>& out) const = 0;
  virtual void enumerateDefs(const compiler::backend::mir::MachineInstruction& instruction,
                             std::vector<compiler::backend::mir::MachineOperand>& out) const = 0;
  virtual void enumeratePhysicalRegisters(
      const compiler::backend::mir::MachineInstruction& instruction,
      std::vector<compiler::backend::mir::MachineOperand>& out) const = 0;

  virtual void replaceUse(compiler::backend::mir::MachineInstruction& instruction,
                          const std::string& from, compiler::backend::mir::MachineOperand to) const = 0;
  virtual void replaceDef(compiler::backend::mir::MachineInstruction& instruction,
                          const std::string& from, compiler::backend::mir::MachineOperand to) const = 0;
};

}  // namespace compiler::backend::target
