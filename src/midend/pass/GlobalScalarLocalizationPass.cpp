#include "../../../include/compiler/midend/pass/GlobalScalarLocalizationPass.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::midend::analysis::FunctionEffectAnalysis;

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

std::string calledFunctionName(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& callee = instruction.operands().front();
  if (callee.size() < 2 || callee.front() != '@') {
    return {};
  }
  return callee.substr(1);
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

std::unordered_set<std::string> collectCacheWorthyCallees(
    const compiler::ir::core::Module& module,
    const FunctionEffectAnalysis& functionEffects) {
  constexpr std::size_t kMinimumInstructionCount = 24;
  std::unordered_set<std::string> result;
  for (const auto& function : module.functions()) {
    std::size_t instructionCount = 0;
    bool directlyRecursive = false;
    for (const auto& block : function.blocks()) {
      instructionCount += block.instructions().size();
      for (const auto& instruction : block.instructions()) {
        directlyRecursive |= calledFunctionName(instruction) == function.name();
      }
    }
    if (functionEffects.isReadNone(function.name()) &&
        (directlyRecursive || instructionCount >= kMinimumInstructionCount)) {
      result.insert(function.name());
    }
  }
  return result;
}

bool feedsCacheWorthyCall(
    const Function& function, const std::string& symbol,
    const std::unordered_set<std::string>& cacheWorthyCallees,
    const FunctionEffectAnalysis& functionEffects) {
  std::unordered_set<std::string> derivedValues;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          instruction.operands().size() == 2 &&
          instruction.operands()[1] == symbol) {
        derivedValues.insert(instruction.result());
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
          continue;
        }
        bool derived = false;
        for (const auto& operand : instruction.operands()) {
          derived |= derivedValues.find(operand) != derivedValues.end();
        }
        if (derived) {
          changed |= derivedValues.insert(instruction.result()).second;
        }
      }
    }
  }

  const std::size_t blockCount = function.blocks().size();
  std::unordered_map<std::string, std::size_t> blockIndex;
  for (std::size_t index = 0; index < blockCount; ++index) {
    blockIndex.emplace(function.blocks()[index].name(), index);
  }
  std::vector<std::vector<bool>> reachable(
      blockCount, std::vector<bool>(blockCount, false));
  std::vector<bool> hasSelfEdge(blockCount, false);
  for (std::size_t index = 0; index < blockCount; ++index) {
    reachable[index][index] = true;
    const auto* terminator = function.blocks()[index].terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto& operands = terminator->operands();
    const std::size_t firstTarget = operands.size() == 1 ? 0 : 1;
    for (std::size_t operand = firstTarget; operand < operands.size(); ++operand) {
      const auto target = blockIndex.find(operands[operand]);
      if (target == blockIndex.end()) {
        continue;
      }
      reachable[index][target->second] = true;
      hasSelfEdge[index] = hasSelfEdge[index] || target->second == index;
    }
  }
  for (std::size_t through = 0; through < blockCount; ++through) {
    for (std::size_t from = 0; from < blockCount; ++from) {
      if (!reachable[from][through]) {
        continue;
      }
      for (std::size_t to = 0; to < blockCount; ++to) {
        reachable[from][to] =
            reachable[from][to] || reachable[through][to];
      }
    }
  }

  for (std::size_t callBlock = 0; callBlock < blockCount; ++callBlock) {
    const auto& block = function.blocks()[callBlock];
    for (const auto& instruction : block.instructions()) {
      if (cacheWorthyCallees.find(calledFunctionName(instruction)) ==
          cacheWorthyCallees.end()) {
        continue;
      }
      bool consumesDerivedValue = false;
      for (std::size_t index = 1; index < instruction.operands().size(); ++index) {
        if (derivedValues.find(instruction.operands()[index]) != derivedValues.end()) {
          consumesDerivedValue = true;
          break;
        }
      }
      if (!consumesDerivedValue) {
        continue;
      }

      std::vector<std::size_t> loopBlocks;
      for (std::size_t candidate = 0; candidate < blockCount; ++candidate) {
        if (reachable[callBlock][candidate] &&
            reachable[candidate][callBlock]) {
          loopBlocks.push_back(candidate);
        }
      }
      if (loopBlocks.size() == 1 && !hasSelfEdge[callBlock]) {
        continue;
      }

      bool clobberedInLoop = false;
      for (const auto loopBlock : loopBlocks) {
        for (const auto& loopInstruction :
             function.blocks()[loopBlock].instructions()) {
          if (loopInstruction.opcode() == "store" &&
              loopInstruction.operands().size() == 3 &&
              loopInstruction.operands()[2] == symbol) {
            clobberedInLoop = true;
          }
          if (loopInstruction.opcode() == "call" &&
              functionEffects.effectFor(calledFunctionName(loopInstruction))
                  .writesExternalMemory) {
            clobberedInLoop = true;
          }
        }
      }
      if (!clobberedInLoop) {
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
                     const std::unordered_map<std::string, std::string>& scalarGlobals,
                     const FunctionEffectAnalysis& functionEffects,
                     const std::unordered_set<std::string>& cacheWorthyCallees) {
  if (function.blocks().empty()) {
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
  const bool usesCalls = containsCall(function);
  for (auto& [symbol, candidate] : candidates) {
    (void)symbol;
    if (!candidate.valid || candidate.accesses == 0 ||
        (candidate.hasStore && candidate.accesses < 2)) {
      continue;
    }
    if (usesCalls &&
        !feedsCacheWorthyCall(function, candidate.symbol, cacheWorthyCallees,
                              functionEffects)) {
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

      if (instruction.opcode() == "call") {
        const auto& effects = functionEffects.effectFor(calledFunctionName(instruction));
        if (effects.readsExternalMemory || effects.writesExternalMemory) {
          for (const auto& candidate : selected) {
            if (!candidate.hasStore) {
              continue;
            }
            const std::string flush = uniqueValueName(
                "global.cache." + candidate.symbol.substr(1) + ".before.call", names);
            rewritten.emplace_back(
                "load", std::vector<std::string>{candidate.type, candidate.slot}, flush);
            rewritten.emplace_back(
                "store", std::vector<std::string>{candidate.type, flush, candidate.symbol});
          }
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

      if (instruction.opcode() == "call") {
        const auto& effects = functionEffects.effectFor(calledFunctionName(instruction));
        if (effects.writesExternalMemory) {
          for (const auto& candidate : selected) {
            const std::string reload = uniqueValueName(
                "global.cache." + candidate.symbol.substr(1) + ".after.call", names);
            rewritten.emplace_back(
                "load", std::vector<std::string>{candidate.type, candidate.symbol}, reload);
            rewritten.emplace_back(
                "store", std::vector<std::string>{candidate.type, reload, candidate.slot});
          }
        }
      }
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
  const FunctionEffectAnalysis functionEffects(module);
  const auto cacheWorthyCallees =
      collectCacheWorthyCallees(module, functionEffects);
  for (auto& function : module.functions()) {
    localizeGlobals(function, scalarGlobals, functionEffects, cacheWorthyCallees);
  }
}

}  // namespace compiler::midend::pass
