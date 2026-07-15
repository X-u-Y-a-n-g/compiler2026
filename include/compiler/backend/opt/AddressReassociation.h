#pragma once

#include "../mir/MachineModule.h"

namespace compiler::backend::opt {

class AddressReassociation {
 public:
  void runReassociation(compiler::backend::mir::MachineModule& module) const;
  void run(compiler::backend::mir::MachineModule& module) const;
};

}  // namespace compiler::backend::opt
