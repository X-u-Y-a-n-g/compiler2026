#pragma once

#include "compiler/backend/mir/MachineModule.h"
#include "compiler/backend/target/TargetInfo.h"
#include "compiler/ir/core/Module.h"

namespace compiler::backend::target {

class TargetLowering {
 public:
  virtual ~TargetLowering() = default;

  [[nodiscard]] virtual compiler::backend::mir::MachineModule lowerModule(
      const compiler::ir::core::Module& module, const TargetInfo& target) const = 0;
};

}  // namespace compiler::backend::target
