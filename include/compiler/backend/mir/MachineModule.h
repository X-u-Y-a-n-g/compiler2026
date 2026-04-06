#pragma once

#include <string>
#include <utility>
#include <vector>

#include "compiler/backend/target/TargetInfo.h"

namespace compiler::backend::mir {

struct MachineInstruction {
  std::string opcode;
  std::vector<std::string> operands;
};

struct MachineBlock {
  std::string name;
  std::vector<MachineInstruction> instructions;
};

struct MachineFunction {
  std::string name;
  std::vector<MachineBlock> blocks;
};

struct MachineModule {
  compiler::backend::target::TargetInfo target;
  std::vector<MachineFunction> functions;
};

}  // namespace compiler::backend::mir
