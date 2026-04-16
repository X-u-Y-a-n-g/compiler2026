#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace compiler::backend::mir {
struct MachineBlock;
struct MachineFunction;
}  // namespace compiler::backend::mir

namespace compiler::backend::common {

struct ControlFlowGraph {
  const compiler::backend::mir::MachineFunction* function = nullptr;
  std::vector<const compiler::backend::mir::MachineBlock*> blocks;
  std::unordered_map<std::string, std::size_t> indexByName;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
};

}  // namespace compiler::backend::common
