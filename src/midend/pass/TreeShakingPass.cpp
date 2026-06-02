#include "../../../include/compiler/midend/pass/TreeShakingPass.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Module;

std::string calleeName(const std::string& operand) {
  return !operand.empty() && operand.front() == '@' ? operand.substr(1) : operand;
}

std::unordered_set<std::string> functionNames(const Module& module) {
  std::unordered_set<std::string> names;
  for (const auto& function : module.functions()) {
    names.insert(function.name());
  }
  return names;
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

std::unordered_set<std::string> reachableFunctions(const Module& module) {
  const auto names = functionNames(module);
  if (names.find("main") == names.end()) {
    return names;
  }

  const auto graph = buildCallGraph(module);
  std::unordered_set<std::string> reachable;
  std::deque<std::string> worklist;
  reachable.insert("main");
  worklist.push_back("main");
  while (!worklist.empty()) {
    const auto current = worklist.front();
    worklist.pop_front();
    const auto found = graph.find(current);
    if (found == graph.end()) {
      continue;
    }
    for (const auto& callee : found->second) {
      if (reachable.insert(callee).second) {
        worklist.push_back(callee);
      }
    }
  }
  return reachable;
}

bool runTreeShaking(Module& module) {
  const auto reachable = reachableFunctions(module);
  const auto oldSize = module.functions().size();
  module.functions().erase(
      std::remove_if(module.functions().begin(), module.functions().end(),
                     [&](const Function& function) {
                       return reachable.find(function.name()) == reachable.end();
                     }),
      module.functions().end());
  return oldSize != module.functions().size();
}

}  // namespace

bool TreeShakingPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  runTreeShaking(module);
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
