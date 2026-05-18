#pragma once

#include "MachineModule.h"
#include "TargetInfo.h"
#include "Module.h"

namespace compiler::backend::target {

class TargetLowering {
 public:
  virtual ~TargetLowering() = default;

  [[nodiscard]] virtual compiler::backend::mir::MachineModule lowerModule(
      const compiler::ir::core::Module& module, const TargetInfo& target) const = 0;
};

}  // namespace compiler::backend::target
