#pragma once

#include <string>

#include "compiler/backend/target/TargetInfo.h"
#include "compiler/backend/target/TargetInstrAdapter.h"
#include "compiler/backend/target/TargetLowering.h"
#include "compiler/backend/target/TargetRegInfo.h"

namespace compiler::ir::core {
class Module;
}

namespace compiler::backend::target {

class BackendTarget {
 public:
  virtual ~BackendTarget() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual std::string generateAssembly(
      const compiler::ir::core::Module& module) const = 0;
};

const TargetRegInfo& regInfoFor(const TargetInfo& target);
const TargetInstrAdapter& instrAdapterFor(const TargetInfo& target);
const TargetLowering& loweringFor(const TargetInfo& target);
const BackendTarget& backendFor(const TargetInfo& target);

}  // namespace compiler::backend::target
