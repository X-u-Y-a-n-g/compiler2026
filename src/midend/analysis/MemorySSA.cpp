#include "../../../include/compiler/midend/analysis/MemorySSA.h"

#include <algorithm>

#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Module.h"

namespace compiler::midend::analysis {
namespace {

std::string calledFunctionName(const compiler::ir::core::Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) return {};
  const auto& name = instruction.operands()[0];
  return !name.empty() && name.front() == '@' ? name.substr(1) : name;
}

}  // namespace

MemorySSA::MemorySSA(const compiler::ir::core::Module& module,
                     const compiler::ir::core::Function& function,
                     const compiler::ir::analysis::LoopAnalysis& loops,
                     const compiler::ir::analysis::AliasAnalysis& aliases,
                     const FunctionEffectAnalysis& effects)
    : aliases_(aliases) {
  (void)module;
  accesses_.push_back(MemoryAccess{});
  instructionAccesses_.resize(function.blocks().size());
  std::vector<std::optional<std::size_t>> phiAccess(function.blocks().size());
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    instructionAccesses_[block].resize(function.blocks()[block].instructions().size());
    if (loops.predecessors()[block].size() > 1) {
      const auto id = accesses_.size();
      phiAccess[block] = id;
      MemoryAccess phi;
      phi.id = id;
      phi.kind = MemoryAccessKind::Phi;
      phi.blockIndex = block;
      accesses_.push_back(std::move(phi));
    }
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto& instruction = instructions[index];
      MemoryAccess access;
      access.blockIndex = block;
      access.instructionIndex = index;
      if (instruction.opcode() == "load" && instruction.operands().size() == 2) {
        access.kind = MemoryAccessKind::Use;
        access.address = instruction.operands()[1];
      } else if (instruction.opcode() == "store" &&
                 instruction.operands().size() == 3) {
        access.kind = MemoryAccessKind::StoreDef;
        access.address = instruction.operands()[2];
        access.storedValue = instruction.operands()[1];
      } else if (instruction.opcode() == "call") {
        access.callee = calledFunctionName(instruction);
        const auto& callEffects = effects.effectFor(access.callee);
        if (callEffects.isReadNone()) continue;
        const bool writes = callEffects.writesExternalMemory ||
                            callEffects.writesUnknownMemory ||
                            !callEffects.writtenSymbols.empty() ||
                            callEffects.hasOtherSideEffects;
        access.kind = writes ? MemoryAccessKind::CallDef : MemoryAccessKind::Use;
        access.clobbersUnknownMemory = callEffects.writesUnknownMemory ||
                                       callEffects.hasOtherSideEffects;
        access.writtenSymbols = callEffects.writtenSymbols;
      } else {
        continue;
      }
      access.id = accesses_.size();
      instructionAccesses_[block][index] = access.id;
      accesses_.push_back(std::move(access));
    }
  }

  std::vector<std::size_t> outgoing(function.blocks().size(), 0);
  const std::size_t iterationLimit = std::max<std::size_t>(8, function.blocks().size() * 4);
  for (std::size_t iteration = 0; iteration < iterationLimit; ++iteration) {
    bool changed = false;
    for (std::size_t block = 0; block < function.blocks().size(); ++block) {
      const auto& predecessors = loops.predecessors()[block];
      std::size_t current = 0;
      if (predecessors.size() == 1) {
        current = outgoing[predecessors.front()];
      } else if (predecessors.size() > 1) {
        current = outgoing[predecessors.front()];
        bool same = true;
        for (const auto predecessor : predecessors) {
          same &= outgoing[predecessor] == current;
        }
        if (!same && phiAccess[block].has_value()) {
          current = *phiAccess[block];
          auto& phi = accesses_[current];
          std::vector<std::size_t> incoming;
          incoming.reserve(predecessors.size());
          for (const auto predecessor : predecessors) {
            incoming.push_back(outgoing[predecessor]);
          }
          if (phi.incoming != incoming) {
            phi.incoming = std::move(incoming);
            changed = true;
          }
        }
      }
      for (const auto accessId : instructionAccesses_[block]) {
        if (!accessId.has_value()) continue;
        auto& access = accesses_[*accessId];
        if (access.previous != current) {
          access.previous = current;
          changed = true;
        }
        if (access.kind == MemoryAccessKind::StoreDef ||
            access.kind == MemoryAccessKind::CallDef) {
          current = access.id;
        }
      }
      if (outgoing[block] != current) {
        outgoing[block] = current;
        changed = true;
      }
    }
    if (!changed) break;
  }
}

std::optional<std::size_t> MemorySSA::accessAt(std::size_t blockIndex,
                                               std::size_t instructionIndex) const {
  if (blockIndex >= instructionAccesses_.size() ||
      instructionIndex >= instructionAccesses_[blockIndex].size()) {
    return std::nullopt;
  }
  return instructionAccesses_[blockIndex][instructionIndex];
}

std::optional<std::size_t> MemorySSA::reachingStore(
    std::size_t loadAccess) const {
  if (loadAccess >= accesses_.size() ||
      accesses_[loadAccess].kind != MemoryAccessKind::Use ||
      accesses_[loadAccess].address.empty()) {
    return std::nullopt;
  }
  std::unordered_set<std::size_t> visiting;
  return reachingStoreFrom(accesses_[loadAccess].previous,
                           accesses_[loadAccess].address, visiting);
}

std::optional<std::size_t> MemorySSA::reachingStoreFrom(
    std::size_t accessId, const std::string& address,
    std::unordered_set<std::size_t>& visiting) const {
  if (accessId == 0 || accessId >= accesses_.size() ||
      !visiting.insert(accessId).second) {
    return std::nullopt;
  }
  const auto& access = accesses_[accessId];
  std::optional<std::size_t> result;
  if (access.kind == MemoryAccessKind::StoreDef) {
    const auto relation = aliases_.alias(access.address, address);
    if (relation == compiler::ir::analysis::AliasResult::MustAlias) {
      result = access.id;
    } else if (relation == compiler::ir::analysis::AliasResult::NoAlias) {
      result = reachingStoreFrom(access.previous, address, visiting);
    }
  } else if (access.kind == MemoryAccessKind::CallDef) {
    const auto pointer = aliases_.pointerInfo(address);
    const bool localUncaptured = pointer.origin !=
                                     compiler::ir::analysis::PointerOrigin::Unknown &&
                                 !pointer.captured && !pointer.root.empty() &&
                                 pointer.root.front() == '%';
    const bool preservesGlobal = !pointer.root.empty() &&
                                 pointer.root.front() == '@' &&
                                 !access.clobbersUnknownMemory &&
                                 access.writtenSymbols.find(pointer.root) ==
                                     access.writtenSymbols.end();
    if (localUncaptured || preservesGlobal) {
      result = reachingStoreFrom(access.previous, address, visiting);
    }
  } else if (access.kind == MemoryAccessKind::Phi) {
    bool first = true;
    std::optional<std::size_t> common;
    for (const auto incoming : access.incoming) {
      if (incoming == access.id) continue;
      auto pathVisiting = visiting;
      const auto candidate = reachingStoreFrom(incoming, address, pathVisiting);
      if (first) {
        common = candidate;
        first = false;
      } else if (common != candidate) {
        common.reset();
        break;
      }
    }
    result = first ? std::nullopt : common;
  } else {
    result = reachingStoreFrom(access.previous, address, visiting);
  }
  visiting.erase(accessId);
  return result;
}

}  // namespace compiler::midend::analysis
