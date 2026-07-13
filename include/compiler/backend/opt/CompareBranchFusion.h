#pragma once

#include "../mir/MachineModule.h"

namespace compiler::backend::opt {

class CompareBranchFusion {
 public:
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
