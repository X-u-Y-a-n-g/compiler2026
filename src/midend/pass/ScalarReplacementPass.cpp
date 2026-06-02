#include "../../../include/compiler/midend/pass/ScalarReplacementPass.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"
#include "MemoryAccessUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct ArrayCandidate {
  std::string type;
  std::string elementType;
  bool rejected = false;
  std::unordered_set<long long> indexes;
};

struct GepInfo {
  std::string base;
  long long index = 0;
};

bool parseConstantIndex(const std::string& text, long long& value) {
  return detail::parseIntegerLiteral(text, value) && value >= 0;
}

void collectArrayCandidates(
    const Function& function, std::unordered_map<std::string, ArrayCandidate>& arrays,
    std::unordered_map<std::string, GepInfo>& geps) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult() &&
          instruction.operands().size() == 1 && memory::isArrayType(instruction.operands()[0])) {
        arrays.emplace(instruction.result(),
                       ArrayCandidate{instruction.operands()[0],
                                      memory::scalarElementType(instruction.operands()[0]), false,
                                      {}});
      }
    }
  }

  if (arrays.empty()) {
    return;
  }

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (!memory::isGepOpcode(instruction.opcode()) || !instruction.hasResult() ||
          instruction.operands().empty()) {
        continue;
      }
      const auto found = arrays.find(instruction.operands()[0]);
      if (found == arrays.end()) {
        continue;
      }
      if (instruction.operands().size() != 2) {
        found->second.rejected = true;
        continue;
      }
      long long index = 0;
      if (!parseConstantIndex(instruction.operands()[1], index)) {
        found->second.rejected = true;
        continue;
      }
      found->second.indexes.insert(index);
      geps.emplace(instruction.result(), GepInfo{instruction.operands()[0], index});
    }
  }
}

void rejectUnsafeUses(const Function& function,
                      std::unordered_map<std::string, ArrayCandidate>& arrays,
                      const std::unordered_map<std::string, GepInfo>& geps) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      for (std::size_t i = 0; i < operands.size(); ++i) {
        if (const auto array = arrays.find(operands[i]); array != arrays.end()) {
          const bool allowedGepBase =
              memory::isGepOpcode(instruction.opcode()) && instruction.hasResult() && i == 0;
          if (!allowedGepBase) {
            array->second.rejected = true;
          }
        }

        const auto gep = geps.find(operands[i]);
        if (gep == geps.end()) {
          continue;
        }
        const bool allowedLoad =
            instruction.opcode() == "load" && operands.size() == 2 && i == 1;
        const bool allowedStore =
            instruction.opcode() == "store" && operands.size() == 3 && i == 2;
        if (!allowedLoad && !allowedStore) {
          if (const auto array = arrays.find(gep->second.base); array != arrays.end()) {
            array->second.rejected = true;
          }
        }
      }
    }
  }
}

std::unordered_map<std::string, std::string>
createScalarSlots(Function& function, const std::unordered_map<std::string, ArrayCandidate>& arrays) {
  std::unordered_map<std::string, std::string> scalarSlots;
  if (function.blocks().empty()) {
    return scalarSlots;
  }

  std::unordered_set<std::string> usedNames;
  for (const auto& parameter : function.parameters()) {
    usedNames.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        usedNames.insert(instruction.result());
      }
    }
  }
  const auto fresh = [&]() {
    for (std::size_t index = 0;; ++index) {
      const std::string candidate = "%sroa" + std::to_string(index);
      if (usedNames.insert(candidate).second) {
        return candidate;
      }
    }
  };

  auto& entryInstructions = function.blocks().front().instructions();
  std::vector<Instruction> prefix;
  for (const auto& [arrayName, candidate] : arrays) {
    if (candidate.rejected || candidate.indexes.empty() || candidate.indexes.size() > 32) {
      continue;
    }
    for (const auto index : candidate.indexes) {
      const std::string slotName = fresh();
      scalarSlots.emplace(arrayName + "\n" + std::to_string(index), slotName);
      prefix.emplace_back("alloca", std::vector<std::string>{candidate.elementType}, slotName);
    }
  }

  if (!prefix.empty()) {
    entryInstructions.insert(entryInstructions.begin(), prefix.begin(), prefix.end());
  }
  return scalarSlots;
}

void rewriteFunction(Function& function, const std::unordered_map<std::string, ArrayCandidate>& arrays,
                     const std::unordered_map<std::string, GepInfo>& geps,
                     const std::unordered_map<std::string, std::string>& scalarSlots) {
  if (scalarSlots.empty()) {
    return;
  }

  std::unordered_map<std::string, std::string> gepReplacements;
  for (const auto& [gepName, info] : geps) {
    const auto found = scalarSlots.find(info.base + "\n" + std::to_string(info.index));
    if (found != scalarSlots.end()) {
      gepReplacements.emplace(gepName, found->second);
    }
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        const auto found = arrays.find(instruction.result());
        if (found != arrays.end() && !found->second.rejected &&
            found->second.indexes.size() <= 32) {
          continue;
        }
      }
      if (instruction.hasResult() && gepReplacements.find(instruction.result()) != gepReplacements.end()) {
        continue;
      }
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(), gepReplacements),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void runScalarReplacement(Function& function) {
  std::unordered_map<std::string, ArrayCandidate> arrays;
  std::unordered_map<std::string, GepInfo> geps;
  collectArrayCandidates(function, arrays, geps);
  rejectUnsafeUses(function, arrays, geps);
  const auto scalarSlots = createScalarSlots(function, arrays);
  rewriteFunction(function, arrays, geps, scalarSlots);
}

}  // namespace

bool ScalarReplacementPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runScalarReplacement(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
