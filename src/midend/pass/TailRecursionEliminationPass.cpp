#include "../../../include/compiler/midend/pass/TailRecursionEliminationPass.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct TailCallSite {
  std::size_t blockIndex = 0;
  std::vector<std::string> arguments;
};

std::unordered_set<std::string> collectValueNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshValueName(std::unordered_set<std::string>& names, std::size_t index) {
  const std::string stem = "%tail.param." + std::to_string(index);
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string freshBlockName(std::unordered_set<std::string>& names) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "tail.entry." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::vector<TailCallSite> findTailCalls(const Function& function) {
  std::vector<TailCallSite> sites;
  const std::string callee = "@" + function.name();
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size(); ++blockIndex) {
    const auto& instructions = function.blocks()[blockIndex].instructions();
    if (instructions.size() < 2) {
      continue;
    }
    const auto& call = instructions[instructions.size() - 2];
    const auto& ret = instructions.back();
    if (call.opcode() != "call" || !call.hasResult() || call.operands().empty() ||
        call.operands().front() != callee ||
        call.operands().size() != function.parameters().size() + 1 ||
        ret.opcode() != "ret" || ret.operands().size() != 1 ||
        ret.operands().front() != call.result()) {
      continue;
    }
    sites.push_back(TailCallSite{blockIndex, {}});
  }
  return sites;
}

bool eliminateTailCalls(Function& function) {
  auto sites = findTailCalls(function);
  if (sites.empty() || function.blocks().empty() || function.parameters().empty()) {
    return false;
  }

  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const std::string loopHeader = function.blocks().front().name();
  const std::string preheader = freshBlockName(blockNames);

  std::unordered_map<std::string, std::string> parameterReplacements;
  std::vector<std::string> phiResults;
  phiResults.reserve(function.parameters().size());
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    const auto result = freshValueName(valueNames, index);
    parameterReplacements.emplace(function.parameters()[index].name, result);
    phiResults.push_back(result);
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(),
                                                     parameterReplacements),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  for (auto& site : sites) {
    auto& block = function.blocks()[site.blockIndex];
    auto& instructions = block.instructions();
    const auto& call = instructions[instructions.size() - 2];
    site.arguments.assign(call.operands().begin() + 1, call.operands().end());
    instructions.pop_back();
    instructions.pop_back();
    instructions.emplace_back("br", std::vector<std::string>{loopHeader});
  }

  std::vector<Instruction> phis;
  phis.reserve(function.parameters().size());
  for (std::size_t parameterIndex = 0; parameterIndex < function.parameters().size();
       ++parameterIndex) {
    std::vector<std::string> operands{
        function.parameters()[parameterIndex].type,
        function.parameters()[parameterIndex].name,
        preheader,
    };
    for (const auto& site : sites) {
      operands.push_back(site.arguments[parameterIndex]);
      operands.push_back(function.blocks()[site.blockIndex].name());
    }
    phis.emplace_back("phi", std::move(operands), phiResults[parameterIndex]);
  }

  auto& headerInstructions = function.blocks().front().instructions();
  headerInstructions.insert(headerInstructions.begin(), phis.begin(), phis.end());

  BasicBlock entry{preheader};
  entry.addInstruction(Instruction{"br", {loopHeader}});
  function.blocks().push_front(std::move(entry));
  return true;
}

}  // namespace

void TailRecursionEliminationPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    eliminateTailCalls(function);
  }
}

}  // namespace compiler::midend::pass
