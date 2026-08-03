#include "../../../include/compiler/backend/target/TargetBackend.h"

#include <stdexcept>

#include "../../../include/compiler/backend/asm/AsmPrinter.h"
#include "../../../include/compiler/backend/asm/AArch64AsmPrinter.h"
#include "../../../include/compiler/backend/frame/FrameLowering.h"
#include "../../../include/compiler/backend/opt/AddressReassociation.h"
#include "../../../include/compiler/backend/opt/AArch64InstructionCombining.h"
#include "../../../include/compiler/backend/opt/AArch64PostRAVectorization.h"
#include "../../../include/compiler/backend/opt/CompareBranchFusion.h"
#include "../../../include/compiler/backend/opt/MachineDeadCodeElimination.h"
#include "../../../include/compiler/backend/opt/MachineLoopBlockPlacement.h"
#include "../../../include/compiler/backend/opt/MachineTraceBlockPlacement.h"
#include "../../../include/compiler/backend/opt/MachineLoopStrengthReduction.h"
#include "../../../include/compiler/backend/opt/PhiElimination.h"
#include "../../../include/compiler/backend/opt/StackObjectPromotion.h"
#include "../../../include/compiler/backend/regalloc/RegisterAllocator.h"
#include "../../../include/compiler/backend/stack/StackLowering.h"
#include "../../../include/compiler/backend/target/Riscv64Target.h"
#include "../../../include/compiler/backend/target/AArch64Target.h"

namespace compiler::backend::target {

namespace {

class Riscv64Backend final : public BackendTarget {
 public:
  [[nodiscard]] std::string name() const override { return "riscv64"; }

  [[nodiscard]] std::string generateAssembly(
      const compiler::ir::core::Module& module) const override {
    const auto target = TargetInfo::fromName("riscv64");
    auto machineModule = loweringFor(target).lowerModule(module, target);
    compiler::backend::opt::AddressReassociation{}.runReassociation(
        machineModule);
    compiler::backend::opt::MachineLoopStrengthReduction{}.run(machineModule);
    compiler::backend::opt::PhiElimination{}.run(machineModule);
    compiler::backend::opt::CompareBranchFusion{}.run(machineModule);
    compiler::backend::opt::StackObjectPromotion{}.run(machineModule);
    compiler::backend::opt::AddressReassociation{}.run(machineModule);
    compiler::backend::regalloc::RegisterAllocator{}.allocate(machineModule);
    compiler::backend::opt::MachineDeadCodeElimination{}.run(machineModule);
    compiler::backend::frame::FrameLowering{}.lower(machineModule);
    compiler::backend::stack::StackLowering{}.lower(machineModule);
    compiler::backend::opt::MachineLoopBlockPlacement{}.run(machineModule);
    compiler::backend::opt::MachineTraceBlockPlacement{}.run(machineModule);
    return compiler::backend::asmcode::AsmPrinter{}.print(machineModule);
  }
};

class AArch64Backend final : public BackendTarget {
 public:
  [[nodiscard]] std::string name() const override { return "aarch64"; }

  [[nodiscard]] std::string generateAssembly(
      const compiler::ir::core::Module& module) const override {
    const auto target = TargetInfo::fromName("aarch64");
    auto machineModule = loweringFor(target).lowerModule(module, target);
    compiler::backend::opt::AddressReassociation{}.runReassociation(machineModule);
    compiler::backend::opt::MachineLoopStrengthReduction{}.run(machineModule);
    compiler::backend::opt::PhiElimination{}.run(machineModule);
    compiler::backend::opt::CompareBranchFusion{}.run(machineModule);
    compiler::backend::opt::StackObjectPromotion{}.run(machineModule);
    compiler::backend::opt::AddressReassociation{}.run(machineModule);
    compiler::backend::opt::AArch64InstructionCombining{}.run(machineModule);
    compiler::backend::regalloc::RegisterAllocator{}.allocate(machineModule);
    compiler::backend::opt::MachineDeadCodeElimination{}.run(machineModule);
    compiler::backend::frame::FrameLowering{}.lower(machineModule);
    compiler::backend::stack::StackLowering{}.lower(machineModule);
    compiler::backend::opt::MachineLoopBlockPlacement{}.run(machineModule);
    compiler::backend::opt::MachineTraceBlockPlacement{}.run(machineModule);
    compiler::backend::opt::AArch64InstructionCombining{}.runPostRA(
        machineModule);
    compiler::backend::opt::AArch64PostRAVectorization{}.run(machineModule);
    return compiler::backend::asmcode::AArch64AsmPrinter{}.print(machineModule);
  }
};

}  // namespace

const TargetRegInfo& regInfoFor(const TargetInfo& target) {
  static const riscv64::RegInfo riscv64;
  static const aarch64::RegInfo aarch64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  if (target.name() == "aarch64") return aarch64;
  throw std::runtime_error("target reg info not implemented for " +
                           target.name());
}

const TargetInstrAdapter& instrAdapterFor(const TargetInfo& target) {
  static const riscv64::InstrAdapter riscv64;
  static const aarch64::InstrAdapter aarch64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  if (target.name() == "aarch64") return aarch64;
  throw std::runtime_error("target instr adapter not implemented for " +
                           target.name());
}

const TargetLowering& loweringFor(const TargetInfo& target) {
  static const riscv64::Lowering riscv64;
  static const aarch64::Lowering aarch64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  if (target.name() == "aarch64") return aarch64;
  throw std::runtime_error("target lowering not implemented for " +
                           target.name());
}

const BackendTarget& backendFor(const TargetInfo& target) {
  static const Riscv64Backend riscv64;
  static const AArch64Backend aarch64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  if (target.name() == "aarch64") return aarch64;
  throw std::runtime_error("target backend not implemented for " +
                           target.name());
}

}  // namespace compiler::backend::target
