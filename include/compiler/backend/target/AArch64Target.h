#pragma once

#include <string>
#include <vector>

#include "TargetInstrAdapter.h"
#include "TargetLowering.h"
#include "TargetRegInfo.h"

namespace compiler::backend::target::aarch64 {

class RegInfo final : public TargetRegInfo {
 public:
  [[nodiscard]] const std::string& stackPointerRegister() const override { return sp_; }
  [[nodiscard]] const std::string& framePointerRegister() const override { return fp_; }
  [[nodiscard]] const std::string& returnAddressRegister() const override { return lr_; }
  [[nodiscard]] const std::string& zeroRegister() const override { return zero_; }
  [[nodiscard]] const std::vector<std::string>& integerArgumentRegisters() const override { return intArgs_; }
  [[nodiscard]] const std::vector<std::string>& floatArgumentRegisters() const override { return floatArgs_; }
  [[nodiscard]] const std::vector<std::string>& calleeSavedIntegerRegisters() const override { return calleeSavedInt_; }
  [[nodiscard]] const std::vector<std::string>& calleeSavedFloatRegisters() const override { return calleeSavedFloat_; }
  [[nodiscard]] const std::vector<std::string>& reservedRegisters() const override { return reserved_; }
  [[nodiscard]] const std::vector<std::string>& allocatableIntegerRegisters() const override { return allocatableInt_; }
  [[nodiscard]] const std::vector<std::string>& allocatableFloatRegisters() const override { return allocatableFloat_; }
  [[nodiscard]] const std::vector<std::string>& scratchIntegerRegisters() const override { return scratchInt_; }
  [[nodiscard]] const std::vector<std::string>& scratchFloatRegisters() const override { return scratchFloat_; }

 private:
  std::string sp_ = "sp";
  std::string fp_ = "x29";
  std::string lr_ = "x30";
  std::string zero_ = "xzr";
  std::vector<std::string> intArgs_ = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
  std::vector<std::string> floatArgs_ = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
  std::vector<std::string> calleeSavedInt_ = {"x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"};
  std::vector<std::string> calleeSavedFloat_ = {"s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15"};
  std::vector<std::string> reserved_ = {"sp", "xzr", "x15", "x16", "x17", "x18", "x29", "x30", "s16", "s17", "s18"};
  std::vector<std::string> allocatableInt_ = {
      "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28",
      "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14"};
  std::vector<std::string> allocatableFloat_ = {
      "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
      "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
      "s19", "s20", "s21", "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31"};
  std::vector<std::string> scratchInt_ = {"x15", "x16", "x17"};
  std::vector<std::string> scratchFloat_ = {"s16", "s17", "s18"};
};

class InstrAdapter final : public TargetInstrAdapter {
 public:
  [[nodiscard]] bool isCall(const compiler::backend::mir::MachineInstruction&) const override;
  [[nodiscard]] bool isReturn(const compiler::backend::mir::MachineInstruction&) const override;
  [[nodiscard]] bool isUnconditionalBranch(const compiler::backend::mir::MachineInstruction&) const override;
  [[nodiscard]] bool isConditionalBranch(const compiler::backend::mir::MachineInstruction&) const override;
  [[nodiscard]] std::vector<std::string> branchTargets(const compiler::backend::mir::MachineInstruction&) const override;
  void enumerateUses(const compiler::backend::mir::MachineInstruction&, std::vector<compiler::backend::mir::MachineOperand>&) const override;
  void enumerateDefs(const compiler::backend::mir::MachineInstruction&, std::vector<compiler::backend::mir::MachineOperand>&) const override;
  void enumeratePhysicalRegisters(const compiler::backend::mir::MachineInstruction&, std::vector<compiler::backend::mir::MachineOperand>&) const override;
  void replaceUse(compiler::backend::mir::MachineInstruction&, const std::string&, compiler::backend::mir::MachineOperand) const override;
  void replaceDef(compiler::backend::mir::MachineInstruction&, const std::string&, compiler::backend::mir::MachineOperand) const override;
};

class Lowering final : public TargetLowering {
 public:
  [[nodiscard]] compiler::backend::mir::MachineModule lowerModule(
      const compiler::ir::core::Module&, const compiler::backend::target::TargetInfo&) const override;
};

}  // namespace compiler::backend::target::aarch64
