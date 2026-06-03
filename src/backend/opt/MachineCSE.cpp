#include "../../../include/compiler/backend/opt/MachineCSE.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"
#include "MachineOptUtils.h"

namespace compiler::backend::opt {

namespace {

constexpr std::size_t kMaxExpressionsPerBlock = 2048;
constexpr std::size_t kMaxInstructionsForCSE = 8192;

struct RegisterDependency {
  std::string key;
  std::size_t generation = 0;
};

struct ExpressionEntry {
  compiler::backend::mir::MachineOperand result;
  std::string resultKey;
  std::size_t resultGeneration = 0;
  std::vector<RegisterDependency> dependencies;
};

std::string expressionKey(const compiler::backend::mir::MachineInstruction& instruction) {
  std::string key = instruction.opcode;
  for (const auto& operand : instruction.operands) {
    key += "|";
    key += detail::operandKey(operand);
  }
  return key;
}

bool canCSE(const compiler::backend::mir::MachineInstruction& instruction,
            const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (!detail::isPureInstruction(instruction, adapter)) {
    return false;
  }
  if (instruction.kind != compiler::backend::mir::MachineInstructionKind::Target) {
    return false;
  }
  if (instruction.opcode == "div" || instruction.opcode == "sdiv" || instruction.opcode == "mod" ||
      instruction.opcode == "srem" || instruction.opcode == "fdiv" ||
      instruction.opcode.rfind("fcmp.", 0) == 0) {
    return false;
  }
  return !detail::opcodeIsMemoryAccess(instruction.opcode);
}

std::size_t currentGeneration(const std::string& key,
                              const std::unordered_map<std::string, std::size_t>& generations) {
  const auto found = generations.find(key);
  return found == generations.end() ? 0 : found->second;
}

bool isValidExpression(const ExpressionEntry& entry,
                       const std::unordered_map<std::string, std::size_t>& generations) {
  if (!entry.resultKey.empty() &&
      currentGeneration(entry.resultKey, generations) != entry.resultGeneration) {
    return false;
  }
  for (const auto& dependency : entry.dependencies) {
    if (currentGeneration(dependency.key, generations) != dependency.generation) {
      return false;
    }
  }
  return true;
}

ExpressionEntry makeExpressionEntry(
    const compiler::backend::mir::MachineInstruction& instruction,
    const std::unordered_map<std::string, std::size_t>& generations) {
  ExpressionEntry entry;
  entry.result = instruction.result;
  entry.resultKey = detail::regKey(instruction.result);
  entry.resultGeneration = currentGeneration(entry.resultKey, generations);
  for (const auto& operand : instruction.operands) {
    const auto key = detail::regKey(operand);
    if (!key.empty()) {
      entry.dependencies.push_back(RegisterDependency{key, currentGeneration(key, generations)});
    }
  }
  return entry;
}

}  // namespace

void MachineCSE::run(compiler::backend::mir::MachineModule& module) const {
  const auto& adapter = compiler::backend::target::instrAdapterFor(module.target);

  for (auto& function : module.functions) {
    for (auto& block : function.blocks) {
      if (block.instructions.size() > kMaxInstructionsForCSE) {
        continue;
      }

      std::unordered_map<std::string, ExpressionEntry> expressions;
      std::unordered_map<std::string, std::size_t> generations;

      for (auto& instruction : block.instructions) {
        if (adapter.isCall(instruction) || instruction.opcode == "store") {
          expressions.clear();
        }

        for (const auto& def : detail::defsOf(instruction, adapter)) {
          const auto key = detail::regKey(def);
          if (!key.empty()) {
            ++generations[key];
          }
        }

        if (!canCSE(instruction, adapter)) {
          continue;
        }

        const auto key = expressionKey(instruction);
        auto found = expressions.find(key);
        if (found != expressions.end()) {
          if (isValidExpression(found->second, generations)) {
            instruction = compiler::backend::mir::MachineInstruction::copy(instruction.result,
                                                                           found->second.result);
            continue;
          }
          expressions.erase(found);
        }

        if (expressions.size() >= kMaxExpressionsPerBlock) {
          expressions.clear();
        }
        expressions.emplace(key, makeExpressionEntry(instruction, generations));
      }
    }
  }
}

}  // namespace compiler::backend::opt
