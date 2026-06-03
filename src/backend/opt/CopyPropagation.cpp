#include "../../../include/compiler/backend/opt/CopyPropagation.h"

#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

constexpr std::size_t kMaxAliasDepth = 16;
constexpr std::size_t kMaxAliasesPerBlock = 4096;

using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;

struct AliasEntry {
  MachineOperand source;
  std::string sourceKey;
  std::size_t sourceGeneration = 0;
};

MachineOperand resolveAlias(const MachineOperand& operand,
                            const std::unordered_map<std::string, AliasEntry>& aliases,
                            const std::unordered_map<std::string, std::size_t>& generations) {
  if (!detail::isRegisterLike(operand)) {
    return operand;
  }

  MachineOperand current = operand;
  std::unordered_set<std::string> seen;
  std::size_t depth = 0;
  while (detail::isRegisterLike(current)) {
    if (++depth > kMaxAliasDepth) {
      return current;
    }
    const auto key = detail::regKey(current);
    if (!seen.insert(key).second) {
      return operand;
    }
    const auto found = aliases.find(key);
    if (found == aliases.end()) {
      return current;
    }
    if (!found->second.sourceKey.empty()) {
      const auto generation = generations.find(found->second.sourceKey);
      const auto currentGeneration = generation == generations.end() ? 0 : generation->second;
      if (currentGeneration != found->second.sourceGeneration) {
        return current;
      }
    }
    current = found->second.source;
  }
  return current;
}

std::size_t currentGeneration(const std::string& key,
                              const std::unordered_map<std::string, std::size_t>& generations) {
  const auto found = generations.find(key);
  return found == generations.end() ? 0 : found->second;
}

}  // namespace

void CopyPropagation::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      std::unordered_map<std::string, AliasEntry> aliases;
      std::unordered_map<std::string, std::size_t> generations;
      std::vector<MachineInstruction> rewritten;
      rewritten.reserve(block.instructions.size());

      for (auto instruction : block.instructions) {
        if (adapter.isCall(instruction)) {
          aliases.clear();
        }

        for (const auto& use : detail::usesOf(instruction, adapter)) {
          const auto replacement = resolveAlias(use, aliases, generations);
          if (!detail::sameOperand(use, replacement)) {
            detail::replaceUseByKey(instruction, detail::regKey(use), replacement, adapter);
          }
        }

        for (const auto& def : detail::defsOf(instruction, adapter)) {
          const auto key = detail::regKey(def);
          if (!key.empty()) {
            ++generations[key];
            aliases.erase(key);
          }
        }

        if (instruction.kind == MachineInstructionKind::Copy && instruction.hasResult &&
            !instruction.operands.empty()) {
          const auto source = resolveAlias(instruction.operands[0], aliases, generations);
          instruction.operands[0] = source;
          if (detail::sameOperand(instruction.result, source)) {
            continue;
          }
          if (detail::isRegisterLike(instruction.result)) {
            const auto sourceKey = detail::regKey(source);
            if (aliases.size() >= kMaxAliasesPerBlock) {
              aliases.clear();
            }
            aliases[detail::regKey(instruction.result)] =
                AliasEntry{source, sourceKey, currentGeneration(sourceKey, generations)};
          }
        }

        rewritten.push_back(std::move(instruction));
      }

      block.instructions = std::move(rewritten);
    }
  }
}

}  // namespace compiler::backend::opt
