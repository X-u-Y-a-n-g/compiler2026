#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../ir/analysis/AliasAnalysis.h"
#include "../../ir/analysis/LoopAnalysis.h"
#include "FunctionEffectAnalysis.h"

namespace compiler::ir::core {
class Function;
class Module;
}

namespace compiler::midend::analysis {

enum class MemoryAccessKind { LiveOnEntry, Use, StoreDef, CallDef, Phi };

struct MemoryAccess {
  std::size_t id = 0;
  MemoryAccessKind kind = MemoryAccessKind::LiveOnEntry;
  std::size_t blockIndex = 0;
  std::size_t instructionIndex = 0;
  std::size_t previous = 0;
  std::vector<std::size_t> incoming;
  std::string address;
  std::string storedValue;
  std::string callee;
  bool clobbersUnknownMemory = false;
  std::unordered_set<std::string> writtenSymbols;
};

class MemorySSA {
 public:
  MemorySSA(const compiler::ir::core::Module& module,
            const compiler::ir::core::Function& function,
            const compiler::ir::analysis::LoopAnalysis& loops,
            const compiler::ir::analysis::AliasAnalysis& aliases,
            const FunctionEffectAnalysis& effects);

  [[nodiscard]] const std::vector<MemoryAccess>& accesses() const {
    return accesses_;
  }
  [[nodiscard]] std::optional<std::size_t> accessAt(
      std::size_t blockIndex, std::size_t instructionIndex) const;
  [[nodiscard]] std::optional<std::size_t> reachingStore(
      std::size_t loadAccess) const;

 private:
  std::optional<std::size_t> reachingStoreFrom(
      std::size_t access, const std::string& address,
      std::unordered_set<std::size_t>& visiting) const;

  const compiler::ir::analysis::AliasAnalysis& aliases_;
  std::vector<MemoryAccess> accesses_;
  std::vector<std::vector<std::optional<std::size_t>>> instructionAccesses_;
};

}  // namespace compiler::midend::analysis
