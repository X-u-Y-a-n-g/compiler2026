#include "../../../include/compiler/midend/pass/GlobalScalarLocalizationPass.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct Candidate {
  std::string symbol;
  std::string type;
  std::string slot;
  std::string initial;
  std::size_t accesses = 0;
  bool hasStore = false;
  bool valid = true;
};

bool isScalarType(const std::string& type) { return type.find('[') == std::string::npos; }

std::string uniqueValueName(const std::string& stem, std::unordered_set<std::string>& names) {
  std::string candidate = "%" + stem;
  std::size_t suffix = 0;
  while (!names.insert(candidate).second) {
    candidate = "%" + stem + "." + std::to_string(++suffix);
  }
  return candidate;
}

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

bool containsCall(const Function& function) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "call") {
        return true;
      }
    }
  }
  return false;
}

void inspectInstruction(const Instruction& instruction,
                        std::unordered_map<std::string, Candidate>& candidates) {
  const auto& operands = instruction.operands();
  for (const auto& operand : operands) {
    auto found = candidates.find(operand);
    if (found == candidates.end()) {
      continue;
    }
    auto& candidate = found->second;
    const bool directLoad = instruction.opcode() == "load" && operands.size() == 2 &&
                            operands[1] == candidate.symbol;
    const bool directStore = instruction.opcode() == "store" && operands.size() == 3 &&
                             operands[2] == candidate.symbol;
    if (!directLoad && !directStore) {
      candidate.valid = false;
      continue;
    }
    ++candidate.accesses;
    candidate.hasStore |= directStore;
  }
}

void localizeGlobals(Function& function,
                     const std::unordered_map<std::string, std::string>& scalarGlobals) {
  if (function.blocks().empty() || containsCall(function)) {
    return;
  }

  std::unordered_map<std::string, Candidate> candidates;
  for (const auto& [symbol, type] : scalarGlobals) {
    candidates.emplace(symbol, Candidate{symbol, type, "", "", 0, false, true});
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      inspectInstruction(instruction, candidates);
    }
  }

  auto names = collectValueNames(function);
  std::vector<Candidate> selected;
  for (auto& [symbol, candidate] : candidates) {
    (void)symbol;
    if (!candidate.valid || candidate.accesses == 0 ||
        (candidate.hasStore && candidate.accesses < 2)) {
      continue;
    }
    std::string stem = "global.cache." + candidate.symbol.substr(1);
    candidate.slot = uniqueValueName(stem + ".addr", names);
    candidate.initial = uniqueValueName(stem + ".initial", names);
    selected.push_back(std::move(candidate));
  }
  std::sort(selected.begin(), selected.end(), [](const Candidate& lhs, const Candidate& rhs) {
    return lhs.symbol < rhs.symbol;
  });
  if (selected.empty()) {
    return;
  }

  std::unordered_map<std::string, const Candidate*> selectedBySymbol;
  for (const auto& candidate : selected) {
    selectedBySymbol.emplace(candidate.symbol, &candidate);
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size() + selected.size() * 2);
    for (const auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      if (instruction.opcode() == "load" && operands.size() == 2) {
        const auto found = selectedBySymbol.find(operands[1]);
        if (found != selectedBySymbol.end()) {
          operands[1] = found->second->slot;
        }
      } else if (instruction.opcode() == "store" && operands.size() == 3) {
        const auto found = selectedBySymbol.find(operands[2]);
        if (found != selectedBySymbol.end()) {
          operands[2] = found->second->slot;
        }
      }

      if (instruction.opcode() == "ret") {
        for (const auto& candidate : selected) {
          if (!candidate.hasStore) {
            continue;
          }
          const std::string flush = uniqueValueName(
              "global.cache." + candidate.symbol.substr(1) + ".flush", names);
          rewritten.emplace_back("load", std::vector<std::string>{candidate.type, candidate.slot},
                                 flush);
          rewritten.emplace_back(
              "store", std::vector<std::string>{candidate.type, flush, candidate.symbol});
        }
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  std::vector<Instruction> initialization;
  initialization.reserve(selected.size() * 3);
  for (const auto& candidate : selected) {
    initialization.emplace_back("alloca", std::vector<std::string>{candidate.type}, candidate.slot);
    initialization.emplace_back(
        "load", std::vector<std::string>{candidate.type, candidate.symbol}, candidate.initial);
    initialization.emplace_back(
        "store", std::vector<std::string>{candidate.type, candidate.initial, candidate.slot});
  }
  auto& entry = function.blocks().front().instructions();
  entry.insert(entry.begin(), initialization.begin(), initialization.end());
}

}  // namespace

void GlobalScalarLocalizationPass::run(compiler::ir::core::Module& module) {
  std::unordered_map<std::string, std::string> scalarGlobals;
  for (const auto& global : module.globals()) {
    if (isScalarType(global.type())) {
      scalarGlobals.emplace("@" + global.name(), global.type());
    }
  }
  for (auto& function : module.functions()) {
    localizeGlobals(function, scalarGlobals);
  }
}

}  // namespace compiler::midend::pass
