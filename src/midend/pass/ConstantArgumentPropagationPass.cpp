#include "../../../include/compiler/midend/pass/ConstantArgumentPropagationPass.h"

#include <cstddef>
#include <optional>
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
using compiler::ir::core::Module;

using ConstantMap = std::unordered_map<std::string, std::string>;
using DefinitionMap = std::unordered_map<std::string, const Instruction*>;

ConstantMap immutableScalarGlobals(const Module& module) {
  ConstantMap candidates;
  for (const auto& global : module.globals()) {
    long long value = 0;
    if (global.type() == "int" && global.initializer().isScalar() &&
        detail::parseIntegerLiteral(global.initializer().literal(), value)) {
      candidates.emplace("@" + global.name(), std::to_string(value));
    }
  }

  std::unordered_set<std::string> invalid;
  for (const auto& function : module.functions()) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        const auto& operands = instruction.operands();
        for (std::size_t index = 0; index < operands.size(); ++index) {
          if (candidates.find(operands[index]) == candidates.end()) continue;
          const bool directScalarLoad =
              instruction.opcode() == "load" && operands.size() == 2 &&
              index == 1 && operands[0] == "int";
          if (!directScalarLoad) invalid.insert(operands[index]);
        }
      }
    }
  }
  for (const auto& symbol : invalid) candidates.erase(symbol);
  return candidates;
}

DefinitionMap definitions(const Function& function) {
  DefinitionMap result;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        result.emplace(instruction.result(), &instruction);
      }
    }
  }
  return result;
}

std::optional<std::string> resolveIntegerConstant(
    const std::string& operand, const DefinitionMap& definitions,
    const ConstantMap& immutableGlobals) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(operand, literal)) {
    return std::to_string(literal);
  }
  const auto found = definitions.find(operand);
  if (found == definitions.end()) return std::nullopt;
  const auto& instruction = *found->second;
  if (instruction.opcode() != "load" ||
      instruction.operands().size() != 2 ||
      instruction.operands()[0] != "int") {
    return std::nullopt;
  }
  const auto global = immutableGlobals.find(instruction.operands()[1]);
  return global == immutableGlobals.end()
             ? std::nullopt
             : std::optional<std::string>{global->second};
}

struct ParameterConsensus {
  std::size_t calls = 0;
  std::vector<std::optional<std::string>> constants;
  std::vector<bool> valid;
};

std::unordered_map<std::string, ParameterConsensus> collectConsensus(
    const Module& module, const ConstantMap& immutableGlobals) {
  std::unordered_map<std::string, std::size_t> arities;
  std::unordered_map<std::string, ParameterConsensus> result;
  for (const auto& function : module.functions()) {
    if (function.name() == "main") continue;
    const std::size_t arity = function.parameters().size();
    arities.emplace(function.name(), arity);
    result.emplace(function.name(),
                   ParameterConsensus{0,
                                      std::vector<std::optional<std::string>>(
                                          arity),
                                      std::vector<bool>(arity, true)});
  }

  for (const auto& caller : module.functions()) {
    const auto callerDefinitions = definitions(caller);
    for (const auto& block : caller.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" ||
            instruction.operands().empty()) {
          continue;
        }
        const auto& symbol = instruction.operands().front();
        if (symbol.size() < 2 || symbol.front() != '@') continue;
        const std::string callee = symbol.substr(1);
        const auto arity = arities.find(callee);
        const auto consensus = result.find(callee);
        if (arity == arities.end() || consensus == result.end() ||
            instruction.operands().size() != arity->second + 1) {
          continue;
        }
        auto& state = consensus->second;
        ++state.calls;
        for (std::size_t index = 0; index < arity->second; ++index) {
          if (!state.valid[index]) continue;
          const auto constant = resolveIntegerConstant(
              instruction.operands()[index + 1], callerDefinitions,
              immutableGlobals);
          if (!constant.has_value()) {
            state.valid[index] = false;
          } else if (!state.constants[index].has_value()) {
            state.constants[index] = *constant;
          } else if (*state.constants[index] != *constant) {
            state.valid[index] = false;
          }
        }
      }
    }
  }
  return result;
}

void propagateConstants(Function& function,
                        const ParameterConsensus& consensus) {
  if (consensus.calls == 0 ||
      consensus.constants.size() != function.parameters().size()) {
    return;
  }
  std::unordered_map<std::string, std::string> replacements;
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    if (function.parameters()[index].type == "int" &&
        consensus.valid[index] && consensus.constants[index].has_value()) {
      replacements.emplace(function.parameters()[index].name,
                           *consensus.constants[index]);
    }
  }
  if (replacements.empty()) return;

  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      bool changed = false;
      for (auto& operand : operands) {
        const auto replacement = replacements.find(operand);
        if (replacement == replacements.end()) continue;
        operand = replacement->second;
        changed = true;
      }
      if (changed) {
        instruction = Instruction{instruction.opcode(), std::move(operands),
                                  instruction.result()};
      }
    }
  }
}

}  // namespace

void ConstantArgumentPropagationPass::run(
    compiler::ir::core::Module& module) {
  const auto immutableGlobals = immutableScalarGlobals(module);
  const auto consensus = collectConsensus(module, immutableGlobals);
  for (auto& function : module.functions()) {
    const auto found = consensus.find(function.name());
    if (found != consensus.end()) propagateConstants(function, found->second);
  }
}

}  // namespace compiler::midend::pass
