#include "../../../include/compiler/midend/pass/FunctionSpecializationPass.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/core/BasicBlock.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::FunctionParameter;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr std::size_t kMaxSpecializationsPerModule = 16;
constexpr std::size_t kMaxSpecializationsPerFunction = 4;
constexpr std::size_t kMaxSpecializedInstructionCount = 96;

struct Specialization {
  std::string key;
  std::string callee;
  std::string cloneName;
  std::unordered_map<std::string, std::string> parameterConstants;
};

std::string calleeName(const std::string& operand) {
  return !operand.empty() && operand.front() == '@' ? operand.substr(1) : operand;
}

bool isConstantOperand(const std::string& operand) {
  long long integer = 0;
  if (detail::parseIntegerLiteral(operand, integer)) {
    return true;
  }
  double floating = 0.0;
  return detail::parseFloatLiteral(operand, floating);
}

std::unordered_set<std::string> functionNames(const Module& module) {
  std::unordered_set<std::string> names;
  for (const auto& function : module.functions()) {
    names.insert(function.name());
  }
  return names;
}

std::unordered_map<std::string, const Function*> functionMap(const Module& module) {
  std::unordered_map<std::string, const Function*> functions;
  for (const auto& function : module.functions()) {
    functions.emplace(function.name(), &function);
  }
  return functions;
}

std::unordered_map<std::string, std::vector<std::string>> buildCallGraph(const Module& module) {
  const auto names = functionNames(module);
  std::unordered_map<std::string, std::vector<std::string>> graph;
  for (const auto& function : module.functions()) {
    auto& callees = graph[function.name()];
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }
        const auto callee = calleeName(instruction.operands().front());
        if (names.find(callee) != names.end()) {
          callees.push_back(callee);
        }
      }
    }
  }
  return graph;
}

std::unordered_set<std::string> recursiveFunctions(const Module& module) {
  const auto graph = buildCallGraph(module);
  std::unordered_set<std::string> recursive;
  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack;

  std::function<void(const std::string&)> dfs = [&](const std::string& function) {
    if (visiting.find(function) != visiting.end()) {
      const auto start = std::find(stack.begin(), stack.end(), function);
      for (auto it = start; it != stack.end(); ++it) {
        recursive.insert(*it);
      }
      return;
    }
    if (visited.find(function) != visited.end()) {
      return;
    }

    visiting.insert(function);
    stack.push_back(function);
    if (const auto found = graph.find(function); found != graph.end()) {
      for (const auto& callee : found->second) {
        dfs(callee);
      }
    }
    stack.pop_back();
    visiting.erase(function);
    visited.insert(function);
  };

  for (const auto& function : module.functions()) {
    dfs(function.name());
  }
  return recursive;
}

std::size_t instructionCount(const Function& function) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) {
    count += block.instructions().size();
  }
  return count;
}

std::string specializationKey(const Function& callee,
                              const std::unordered_map<std::string, std::string>& constants) {
  std::string key = callee.name();
  for (const auto& parameter : callee.parameters()) {
    const auto found = constants.find(parameter.name);
    if (found != constants.end()) {
      key += "\n" + parameter.name + "=" + found->second;
    }
  }
  return key;
}

std::string freshFunctionName(const std::string& callee, std::unordered_set<std::string>& used) {
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = callee + ".spec" + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::vector<Specialization> collectSpecializations(const Module& module) {
  const auto functions = functionMap(module);
  auto usedNames = functionNames(module);
  const auto recursive = recursiveFunctions(module);
  std::unordered_map<std::string, std::string> cloneByKey;
  std::unordered_map<std::string, std::size_t> cloneCountByFunction;
  std::vector<Specialization> specializations;

  for (const auto& caller : module.functions()) {
    for (const auto& block : caller.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (specializations.size() >= kMaxSpecializationsPerModule) {
          return specializations;
        }
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }

        const auto calleeNameText = calleeName(instruction.operands().front());
        const auto calleeIt = functions.find(calleeNameText);
        if (calleeIt == functions.end() || calleeNameText == "main" ||
            recursive.find(calleeNameText) != recursive.end()) {
          continue;
        }
        const auto& callee = *calleeIt->second;
        if (instruction.operands().size() - 1 != callee.parameters().size() ||
            instructionCount(callee) > kMaxSpecializedInstructionCount ||
            cloneCountByFunction[callee.name()] >= kMaxSpecializationsPerFunction) {
          continue;
        }

        std::unordered_map<std::string, std::string> constants;
        for (std::size_t i = 0; i < callee.parameters().size(); ++i) {
          if (callee.parameters()[i].type.find('[') != std::string::npos) {
            continue;
          }
          const auto& argument = instruction.operands()[i + 1];
          if (isConstantOperand(argument)) {
            constants.emplace(callee.parameters()[i].name, argument);
          }
        }
        if (constants.empty()) {
          continue;
        }

        const auto key = specializationKey(callee, constants);
        if (cloneByKey.find(key) != cloneByKey.end()) {
          continue;
        }
        const auto cloneName = freshFunctionName(callee.name(), usedNames);
        cloneByKey.emplace(key, cloneName);
        ++cloneCountByFunction[callee.name()];
        specializations.push_back(Specialization{key, callee.name(), cloneName, std::move(constants)});
      }
    }
  }
  return specializations;
}

Function cloneSpecializedFunction(const Function& original, const Specialization& specialization) {
  std::vector<FunctionParameter> parameters = original.parameters();
  Function clone{specialization.cloneName, original.returnType(), std::move(parameters)};

  for (const auto& block : original.blocks()) {
    BasicBlock clonedBlock{block.name()};
    for (const auto& instruction : block.instructions()) {
      clonedBlock.addInstruction(Instruction{
          instruction.opcode(),
          detail::rewriteOperands(instruction.operands(), specialization.parameterConstants),
          instruction.result()});
    }
    clone.addBlock(std::move(clonedBlock));
  }
  return clone;
}

void appendSpecializedClones(Module& module, const std::vector<Specialization>& specializations) {
  for (const auto& specialization : specializations) {
    const auto found = std::find_if(module.functions().begin(), module.functions().end(),
                                    [&](const Function& function) {
                                      return function.name() == specialization.callee;
                                    });
    if (found == module.functions().end()) {
      continue;
    }
    module.addFunction(cloneSpecializedFunction(*found, specialization));
  }
}

void rewriteSpecializedCalls(Module& module, const std::vector<Specialization>& specializations) {
  std::unordered_map<std::string, std::string> cloneByKey;
  auto functions = functionMap(module);
  for (const auto& specialization : specializations) {
    cloneByKey.emplace(specialization.key, specialization.cloneName);
  }

  for (auto& function : module.functions()) {
    for (auto& block : function.blocks()) {
      std::vector<Instruction> rewritten;
      rewritten.reserve(block.instructions().size());
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          rewritten.push_back(instruction);
          continue;
        }

        auto operands = instruction.operands();
        const auto callee = calleeName(operands.front());
        const auto calleeIt = functions.find(callee);
        if (calleeIt == functions.end() ||
            operands.size() - 1 != calleeIt->second->parameters().size()) {
          rewritten.push_back(instruction);
          continue;
        }

        std::unordered_map<std::string, std::string> constants;
        for (std::size_t i = 0; i < calleeIt->second->parameters().size(); ++i) {
          if (calleeIt->second->parameters()[i].type.find('[') != std::string::npos) {
            continue;
          }
          if (isConstantOperand(operands[i + 1])) {
            constants.emplace(calleeIt->second->parameters()[i].name, operands[i + 1]);
          }
        }
        if (constants.empty()) {
          rewritten.push_back(instruction);
          continue;
        }

        const auto key = specializationKey(*calleeIt->second, constants);
        const auto clone = cloneByKey.find(key);
        if (clone != cloneByKey.end()) {
          operands[0] = "@" + clone->second;
        }
        rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
      }
      block.instructions() = std::move(rewritten);
    }
  }
}

bool runFunctionSpecialization(Module& module) {
  const auto specializations = collectSpecializations(module);
  if (specializations.empty()) {
    return false;
  }
  appendSpecializedClones(module, specializations);
  rewriteSpecializedCalls(module, specializations);
  return true;
}

}  // namespace

bool FunctionSpecializationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  runFunctionSpecialization(module);
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
