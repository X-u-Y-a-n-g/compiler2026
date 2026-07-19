#include "../../../include/compiler/ir/analysis/AliasAnalysis.h"

#include <unordered_set>

#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Module.h"

namespace compiler::ir::analysis {
namespace {

bool isPointerType(const std::string& type) {
  return type.rfind("ptr(", 0) == 0 || type.find('[') != std::string::npos;
}

bool isGep(const std::string& opcode) {
  return opcode == "gep" || opcode == "gep.linear" ||
         opcode == "getelementptr" || opcode == "getelementptr.linear";
}

bool isDistinctObject(PointerOrigin origin) {
  return origin == PointerOrigin::Global || origin == PointerOrigin::Stack;
}

}  // namespace

AliasAnalysis::AliasAnalysis(const compiler::ir::core::Module& module,
                             const compiler::ir::core::Function& function) {
  for (const auto& global : module.globals()) {
    const std::string name = "@" + global.name();
    pointers_.emplace(name,
                      PointerInfo{PointerOrigin::Global, name, {}, false});
  }
  for (const auto& parameter : function.parameters()) {
    if (isPointerType(parameter.type)) {
      pointers_.emplace(parameter.name,
                        PointerInfo{PointerOrigin::Parameter, parameter.name,
                                    {}, true});
    }
  }

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        pointers_[instruction.result()] =
            PointerInfo{PointerOrigin::Stack, instruction.result(), {}, false};
      }
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult() || !isGep(instruction.opcode()) ||
            instruction.operands().size() != 2 ||
            pointers_.find(instruction.result()) != pointers_.end()) {
          continue;
        }
        const auto base = pointers_.find(instruction.operands()[0]);
        if (base == pointers_.end()) continue;
        PointerInfo derived = base->second;
        derived.origin = PointerOrigin::Derived;
        derived.indices.push_back(instruction.operands()[1]);
        pointers_.emplace(instruction.result(), std::move(derived));
        changed = true;
      }
    }
  }

  std::unordered_set<std::string> capturedRoots;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t index = 0; index < instruction.operands().size(); ++index) {
        const auto pointer = pointers_.find(instruction.operands()[index]);
        if (pointer == pointers_.end()) continue;
        const bool addressUse =
            (instruction.opcode() == "load" && index == 1) ||
            (instruction.opcode() == "store" && index == 2) ||
            (isGep(instruction.opcode()) && index == 0);
        if (!addressUse) capturedRoots.insert(pointer->second.root);
      }
    }
  }
  for (auto& entry : pointers_) {
    entry.second.captured = entry.second.origin == PointerOrigin::Parameter ||
                            capturedRoots.find(entry.second.root) != capturedRoots.end();
  }
}

AliasResult AliasAnalysis::alias(const std::string& lhs,
                                 const std::string& rhs) const {
  if (lhs == rhs) return AliasResult::MustAlias;
  const auto lhsInfo = pointers_.find(lhs);
  const auto rhsInfo = pointers_.find(rhs);
  if (lhsInfo == pointers_.end() || rhsInfo == pointers_.end()) {
    return AliasResult::MayAlias;
  }
  if (lhsInfo->second.root == rhsInfo->second.root) {
    return lhsInfo->second.indices == rhsInfo->second.indices
               ? AliasResult::MustAlias
               : AliasResult::MayAlias;
  }
  const auto lhsRootOrigin = pointers_.find(lhsInfo->second.root);
  const auto rhsRootOrigin = pointers_.find(rhsInfo->second.root);
  const PointerOrigin lhsOrigin = lhsRootOrigin == pointers_.end()
                                      ? lhsInfo->second.origin
                                      : lhsRootOrigin->second.origin;
  const PointerOrigin rhsOrigin = rhsRootOrigin == pointers_.end()
                                      ? rhsInfo->second.origin
                                      : rhsRootOrigin->second.origin;
  if (isDistinctObject(lhsOrigin) && isDistinctObject(rhsOrigin)) {
    return AliasResult::NoAlias;
  }
  if ((lhsOrigin == PointerOrigin::Stack && rhsOrigin == PointerOrigin::Parameter) ||
      (rhsOrigin == PointerOrigin::Stack && lhsOrigin == PointerOrigin::Parameter)) {
    return AliasResult::NoAlias;
  }
  return AliasResult::MayAlias;
}

PointerInfo AliasAnalysis::pointerInfo(const std::string& value) const {
  const auto found = pointers_.find(value);
  return found == pointers_.end() ? PointerInfo{} : found->second;
}

bool AliasAnalysis::isCaptured(const std::string& value) const {
  return pointerInfo(value).captured;
}

}  // namespace compiler::ir::analysis
