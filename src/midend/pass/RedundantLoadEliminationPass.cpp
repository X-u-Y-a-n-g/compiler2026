#include "../../../include/compiler/midend/pass/RedundantLoadEliminationPass.h"

#include <deque>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/analysis/AliasAnalysis.h"
#include "../../../include/compiler/ir/analysis/LoopAnalysis.h"
#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"
#include "../../../include/compiler/midend/analysis/MemorySSA.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

bool withinMemorySsaBudget(const Function& function) {
  constexpr std::size_t kMaximumAnalyzedInstructions = 4096;
  constexpr std::size_t kMaximumAnalyzedBlocks = 512;
  if (function.blocks().size() > kMaximumAnalyzedBlocks) return false;
  std::size_t instructionCount = 0;
  for (const auto& block : function.blocks()) {
    instructionCount += block.instructions().size();
    if (instructionCount > kMaximumAnalyzedInstructions) return false;
  }
  return true;
}

bool isRecursiveFunction(const Module& module, const Function& function) {
  std::unordered_map<std::string, const Function*> functions;
  for (const auto& candidate : module.functions()) {
    functions.emplace(candidate.name(), &candidate);
  }
  std::deque<std::string> worklist{function.name()};
  std::unordered_set<std::string> visited;
  while (!worklist.empty()) {
    const auto currentName = worklist.front();
    worklist.pop_front();
    const auto current = functions.find(currentName);
    if (current == functions.end() || !visited.insert(currentName).second) continue;
    for (const auto& block : current->second->blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) continue;
        std::string callee = instruction.operands().front();
        if (!callee.empty() && callee.front() == '@') callee.erase(callee.begin());
        if (callee == function.name()) return true;
        if (functions.find(callee) != functions.end()) worklist.push_back(callee);
      }
    }
  }
  return false;
}

void rewriteAliases(
    Function& function,
    const std::unordered_map<std::string, std::string>& aliases);

void forwardMemorySsaLoads(
    Module& module, Function& function,
    const compiler::midend::analysis::FunctionEffectAnalysis& effects) {
  if (!withinMemorySsaBudget(function)) return;
  const compiler::ir::analysis::LoopAnalysis loops(function);
  const compiler::ir::analysis::AliasAnalysis aliases(module, function);
  const compiler::midend::analysis::MemorySSA memory(
      module, function, loops, aliases, effects);
  std::unordered_map<std::string, std::string> replacements;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto& instruction = instructions[index];
      if (instruction.opcode() != "load" || !instruction.hasResult()) continue;
      const auto access = memory.accessAt(block, index);
      if (!access.has_value()) continue;
      const auto store = memory.reachingStore(*access);
      if (!store.has_value()) continue;
      const auto& definition = memory.accesses()[*store];
      if (definition.storedValue.empty() ||
          definition.storedValue == instruction.result()) {
        continue;
      }
      replacements.emplace(instruction.result(), definition.storedValue);
    }
  }
  // A single forwarded load often only perturbs register allocation and block
  // layout.  Require enough eliminated memory operations to amortize the
  // longer SSA live ranges on an in-order A53-class core.
  constexpr std::size_t kMinimumForwardedLoads = 4;
  if (replacements.size() < kMinimumForwardedLoads) return;
  rewriteAliases(function, replacements);
  for (auto& block : function.blocks()) {
    std::vector<Instruction> retained;
    retained.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          replacements.find(instruction.result()) != replacements.end()) {
        continue;
      }
      retained.push_back(instruction);
    }
    block.instructions() = std::move(retained);
  }
}

std::string loadKey(const std::vector<std::string>& operands) {
  return operands[0] + "\n" + operands[1];
}

std::unordered_set<std::string> collectNonEscapedScalarSlots(const Function& function) {
  std::unordered_set<std::string> slots;
  std::unordered_set<std::string> escaped;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult() &&
          instruction.operands().size() == 1 &&
          instruction.operands()[0].find('[') == std::string::npos) {
        slots.insert(instruction.result());
      }
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t index = 0; index < instruction.operands().size(); ++index) {
        const auto& operand = instruction.operands()[index];
        if (slots.find(operand) == slots.end()) {
          continue;
        }
        const bool isLoadAddress = instruction.opcode() == "load" && index == 1;
        const bool isStoreAddress = instruction.opcode() == "store" && index == 2;
        if (!isLoadAddress && !isStoreAddress) {
          escaped.insert(operand);
        }
      }
    }
  }
  for (const auto& slot : escaped) {
    slots.erase(slot);
  }
  return slots;
}

void rewriteAliases(Function& function,
                    const std::unordered_map<std::string, std::string>& aliases) {
  if (aliases.empty()) {
    return;
  }
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(), aliases),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void eliminateRedundantLoads(Function& function) {
  std::unordered_map<std::string, std::string> aliases;
  const auto localSlots = collectNonEscapedScalarSlots(function);

  for (auto& block : function.blocks()) {
    std::unordered_map<std::string, std::string> availableLoads;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          operands.size() == 2) {
        const auto key = loadKey(operands);
        const auto available = availableLoads.find(key);
        if (available != availableLoads.end()) {
          aliases[instruction.result()] = detail::resolveAlias(available->second, aliases);
          continue;
        }
        availableLoads.emplace(key, instruction.result());
      }

      if (instruction.opcode() == "store" && operands.size() == 3 &&
          localSlots.find(operands[2]) != localSlots.end()) {
        availableLoads[operands[0] + "\n" + operands[2]] = operands[1];
      } else if (instruction.opcode() == "store" || instruction.opcode() == "call") {
        for (auto available = availableLoads.begin(); available != availableLoads.end();) {
          const auto separator = available->first.find('\n');
          const std::string address =
              separator == std::string::npos ? std::string{} : available->first.substr(separator + 1);
          if (localSlots.find(address) == localSlots.end()) {
            available = availableLoads.erase(available);
          } else {
            ++available;
          }
        }
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  rewriteAliases(function, aliases);
}

}  // namespace

void RedundantLoadEliminationPass::run(compiler::ir::core::Module& module) {
  std::optional<compiler::midend::analysis::FunctionEffectAnalysis> effects;
  for (auto& function : module.functions()) {
    if (withinMemorySsaBudget(function) &&
        !isRecursiveFunction(module, function)) {
      if (!effects.has_value()) effects.emplace(module);
      forwardMemorySsaLoads(module, function, *effects);
    }
    eliminateRedundantLoads(function);
  }
}

}  // namespace compiler::midend::pass
