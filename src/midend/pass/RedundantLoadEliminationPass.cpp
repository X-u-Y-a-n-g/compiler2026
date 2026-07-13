#include "../../../include/compiler/midend/pass/RedundantLoadEliminationPass.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

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
  for (auto& function : module.functions()) {
    eliminateRedundantLoads(function);
  }
}

}  // namespace compiler::midend::pass
