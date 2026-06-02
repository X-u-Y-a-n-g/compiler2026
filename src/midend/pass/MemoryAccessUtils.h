#pragma once

#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Instruction.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compiler::midend::pass::memory {

enum class AddressClass {
  Exact,
  Derived,
  MayAlias,
};

struct AddressInfo {
  AddressClass kind = AddressClass::MayAlias;
  std::string key;
};

inline bool isLoad(const compiler::ir::core::Instruction& instruction) {
  return instruction.opcode() == "load" && instruction.operands().size() == 2;
}

inline bool isStore(const compiler::ir::core::Instruction& instruction) {
  return instruction.opcode() == "store" && instruction.operands().size() == 3;
}

inline bool isGepOpcode(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "getelementptr.linear" || opcode == "gep" ||
         opcode == "gep.linear";
}

inline bool isArrayType(const std::string& type) {
  return type.find('[') != std::string::npos;
}

inline std::string scalarElementType(const std::string& type) {
  const auto bracket = type.find('[');
  return bracket == std::string::npos ? type : type.substr(0, bracket);
}

inline std::unordered_map<std::string, compiler::ir::core::Instruction>
collectDefinitions(const compiler::ir::core::Function& function) {
  std::unordered_map<std::string, compiler::ir::core::Instruction> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), instruction);
      }
    }
  }
  return definitions;
}

inline AddressInfo classifyAddress(
    const std::string& address,
    const std::unordered_map<std::string, compiler::ir::core::Instruction>& definitions) {
  if (address.empty()) {
    return {};
  }
  if (address.front() == '@') {
    return AddressInfo{AddressClass::Exact, address};
  }

  const auto found = definitions.find(address);
  if (found == definitions.end()) {
    return {};
  }

  const auto& instruction = found->second;
  if (instruction.opcode() == "alloca" && instruction.operands().size() == 1 &&
      !isArrayType(instruction.operands().front())) {
    return AddressInfo{AddressClass::Exact, address};
  }
  if (isGepOpcode(instruction.opcode())) {
    return AddressInfo{AddressClass::Derived, address};
  }
  return {};
}

inline AddressInfo loadAddress(
    const compiler::ir::core::Instruction& instruction,
    const std::unordered_map<std::string, compiler::ir::core::Instruction>& definitions) {
  return isLoad(instruction) ? classifyAddress(instruction.operands()[1], definitions)
                             : AddressInfo{};
}

inline AddressInfo storeAddress(
    const compiler::ir::core::Instruction& instruction,
    const std::unordered_map<std::string, compiler::ir::core::Instruction>& definitions) {
  return isStore(instruction) ? classifyAddress(instruction.operands()[2], definitions)
                              : AddressInfo{};
}

template <typename Map>
inline void invalidateForWrite(Map& available, const AddressInfo& address) {
  if (address.kind != AddressClass::Exact) {
    available.clear();
    return;
  }
  available.erase(address.key);
}

inline std::string freshName(const compiler::ir::core::Function& function,
                             const std::string& prefix) {
  std::unordered_set<std::string> used;
  for (const auto& parameter : function.parameters()) {
    used.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }

  for (std::size_t index = 0;; ++index) {
    const std::string candidate = "%" + prefix + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

inline std::vector<std::string> branchTargets(const compiler::ir::core::Instruction* terminator) {
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto& operands = terminator->operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

inline std::unordered_map<std::string, std::vector<std::string>>
computePredecessors(const compiler::ir::core::Function& function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto& block : function.blocks()) {
    predecessors.emplace(block.name(), std::vector<std::string>{});
  }
  for (const auto& block : function.blocks()) {
    for (const auto& target : branchTargets(block.terminator())) {
      predecessors[target].push_back(block.name());
    }
  }
  return predecessors;
}

inline std::unordered_map<std::string, std::size_t>
computeSuccessorCounts(const compiler::ir::core::Function& function) {
  std::unordered_map<std::string, std::size_t> successorCounts;
  for (const auto& block : function.blocks()) {
    successorCounts[block.name()] = branchTargets(block.terminator()).size();
  }
  return successorCounts;
}

}  // namespace compiler::midend::pass::memory
