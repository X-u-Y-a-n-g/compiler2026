#include "../../../include/compiler/backend/target/TargetBackend.h"

#include <stdexcept>

#include "../../../include/compiler/backend/asm/AsmPrinter.h"
#include "../../../include/compiler/backend/frame/FrameLowering.h"
#include "../../../include/compiler/backend/opt/AddressReassociation.h"
#include "../../../include/compiler/backend/opt/CompareBranchFusion.h"
#include "../../../include/compiler/backend/opt/PhiElimination.h"
#include "../../../include/compiler/backend/opt/StackObjectPromotion.h"
#include "../../../include/compiler/backend/regalloc/RegisterAllocator.h"
#include "../../../include/compiler/backend/stack/StackLowering.h"
#include "../../../include/compiler/backend/target/Riscv64Target.h"

namespace compiler::backend::target {

namespace {

class Riscv64Backend final : public BackendTarget {
 public:
  [[nodiscard]] std::string name() const override { return "riscv64"; }

  [[nodiscard]] std::string generateAssembly(const compiler::ir::core::Module& module) const override {
    const auto target = TargetInfo::fromName("riscv64");
    auto machineModule = loweringFor(target).lowerModule(module, target);
    compiler::backend::opt::PhiElimination{}.run(machineModule);
    compiler::backend::opt::CompareBranchFusion{}.run(machineModule);
    compiler::backend::opt::StackObjectPromotion{}.run(machineModule);
    compiler::backend::opt::AddressReassociation{}.run(machineModule);
    compiler::backend::regalloc::RegisterAllocator{}.allocate(machineModule);
    compiler::backend::frame::FrameLowering{}.lower(machineModule);
    compiler::backend::stack::StackLowering{}.lower(machineModule);
    return compiler::backend::asmcode::AsmPrinter{}.print(machineModule);
  }
};

}  // namespace

const TargetRegInfo& regInfoFor(const TargetInfo& target) {
  static const riscv64::RegInfo riscv64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  throw std::runtime_error("target reg info not implemented for " + target.name());
}

const TargetInstrAdapter& instrAdapterFor(const TargetInfo& target) {
  static const riscv64::InstrAdapter riscv64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  throw std::runtime_error("target instr adapter not implemented for " + target.name());
}

const TargetLowering& loweringFor(const TargetInfo& target) {
  static const riscv64::Lowering riscv64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  throw std::runtime_error("target lowering not implemented for " + target.name());
}

const BackendTarget& backendFor(const TargetInfo& target) {
  static const Riscv64Backend riscv64;
  if (target.name() == "riscv64") {
    return riscv64;
  }
  throw std::runtime_error("target backend not implemented for " + target.name());
}

}  // namespace compiler::backend::target
