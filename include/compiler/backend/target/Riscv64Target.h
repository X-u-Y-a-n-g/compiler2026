#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "TargetInstrAdapter.h"
#include "TargetLowering.h"
#include "TargetRegInfo.h"

namespace compiler::backend::target::riscv64 {

class RegInfo final : public TargetRegInfo {
 public:
  [[nodiscard]] const std::string& stackPointerRegister() const override { return sp_; }
  [[nodiscard]] const std::string& framePointerRegister() const override { return fp_; }
  [[nodiscard]] const std::string& returnAddressRegister() const override { return ra_; }
  [[nodiscard]] const std::string& zeroRegister() const override { return zero_; }

  [[nodiscard]] const std::vector<std::string>& integerArgumentRegisters() const override {
    return intArgs_;
  }
  [[nodiscard]] const std::vector<std::string>& floatArgumentRegisters() const override {
    return floatArgs_;
  }
  [[nodiscard]] const std::vector<std::string>& calleeSavedIntegerRegisters() const override {
    return calleeSavedInt_;
  }
  [[nodiscard]] const std::vector<std::string>& calleeSavedFloatRegisters() const override {
    return calleeSavedFloat_;
  }
  [[nodiscard]] const std::vector<std::string>& reservedRegisters() const override {
    return reserved_;
  }
  [[nodiscard]] const std::vector<std::string>& allocatableIntegerRegisters() const override {
    return allocatableInt_;
  }
  [[nodiscard]] const std::vector<std::string>& allocatableFloatRegisters() const override {
    return allocatableFloat_;
  }
  [[nodiscard]] const std::vector<std::string>& scratchIntegerRegisters() const override {
    return scratchInt_;
  }
  [[nodiscard]] const std::vector<std::string>& scratchFloatRegisters() const override {
    return scratchFloat_;
  }

 private:
  std::string sp_ = "sp";
  std::string fp_ = "s0";
  std::string ra_ = "ra";
  std::string zero_ = "x0";
  std::vector<std::string> intArgs_ = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
  std::vector<std::string> floatArgs_ = {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"};
  std::vector<std::string> calleeSavedInt_ = {"s0", "s1", "s2", "s3", "s4", "s5",
                                              "s6", "s7", "s8", "s9", "s10", "s11"};
  std::vector<std::string> calleeSavedFloat_ = {"fs0", "fs1", "fs2", "fs3", "fs4", "fs5",
                                                "fs6", "fs7", "fs8", "fs9", "fs10", "fs11"};
  std::vector<std::string> reserved_ = {"x0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
                                        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
                                        "ft0", "ft1", "ft2"};
  std::vector<std::string> allocatableInt_ = {"s1", "s2", "s3", "s4", "s5", "s6",
                                              "s7", "s8", "s9", "s10", "s11",
                                              "a0", "a1", "a2", "a3", "a4", "a5",
                                              "a6", "a7"};
  std::vector<std::string> allocatableFloat_ = {"fs0", "fs1", "fs2", "fs3", "fs4", "fs5",
                                                "fs6", "fs7", "fs8", "fs9", "fs10", "fs11",
                                                "ft3", "ft4", "ft5", "ft6", "ft7", "ft8"};
  std::vector<std::string> scratchInt_ = {"t3", "t4"};
  std::vector<std::string> scratchFloat_ = {"ft9", "ft10", "ft11"};
};

class InstrAdapter final : public TargetInstrAdapter {
 public:
  [[nodiscard]] bool isCall(
      const compiler::backend::mir::MachineInstruction& instruction) const override;
  [[nodiscard]] bool isReturn(
      const compiler::backend::mir::MachineInstruction& instruction) const override;
  [[nodiscard]] bool isUnconditionalBranch(
      const compiler::backend::mir::MachineInstruction& instruction) const override;
  [[nodiscard]] bool isConditionalBranch(
      const compiler::backend::mir::MachineInstruction& instruction) const override;
  [[nodiscard]] std::vector<std::string> branchTargets(
      const compiler::backend::mir::MachineInstruction& instruction) const override;
  void enumerateUses(const compiler::backend::mir::MachineInstruction& instruction,
                     std::vector<compiler::backend::mir::MachineOperand>& out) const override;
  void enumerateDefs(const compiler::backend::mir::MachineInstruction& instruction,
                     std::vector<compiler::backend::mir::MachineOperand>& out) const override;
  void enumeratePhysicalRegisters(
      const compiler::backend::mir::MachineInstruction& instruction,
      std::vector<compiler::backend::mir::MachineOperand>& out) const override;
  void replaceUse(compiler::backend::mir::MachineInstruction& instruction, const std::string& from,
                  compiler::backend::mir::MachineOperand to) const override;
  void replaceDef(compiler::backend::mir::MachineInstruction& instruction, const std::string& from,
                  compiler::backend::mir::MachineOperand to) const override;

 private:
  static bool isRegisterOperand(const compiler::backend::mir::MachineOperand& operand);
  static bool isResultDef(const compiler::backend::mir::MachineInstruction& instruction);
};

class Lowering final : public TargetLowering {
 public:
  [[nodiscard]] compiler::backend::mir::MachineModule lowerModule(
      const compiler::ir::core::Module& module,
      const compiler::backend::target::TargetInfo& target) const override;
};

}  // namespace compiler::backend::target::riscv64
