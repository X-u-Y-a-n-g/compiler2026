#include "../../../include/compiler/midend/pass/LoopStrengthReductionPass.h"

#include "../../../include/compiler/midend/analysis/LoopAnalysis.h"

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
using compiler::midend::analysis::NaturalLoop;

struct Induction {
  std::string phi;
  std::string type;
  std::string init;
  long long step = 0;
};

struct LsrValue {
  std::string phi;
  std::string next;
};

void seedUsedNames(const Function& function, std::unordered_set<std::string>& used) {
  for (const auto& parameter : function.parameters()) {
    used.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }
}

std::string freshName(const Function& function, const std::string& prefix,
                      std::unordered_set<std::string>& used) {
  if (used.empty()) {
    seedUsedNames(function, used);
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = "%" + prefix + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::unordered_map<std::string, const Instruction*> collectDefinitions(const Function& function) {
  std::unordered_map<std::string, const Instruction*> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions[instruction.result()] = &instruction;
      }
    }
  }
  return definitions;
}

std::optional<long long> matchRecurrenceStep(const Instruction& recurrence,
                                             const std::string& phi) {
  if (recurrence.operands().size() != 2) {
    return std::nullopt;
  }

  long long constant = 0;
  if (recurrence.opcode() == "add") {
    if (recurrence.operands()[0] == phi &&
        detail::parseIntegerLiteral(recurrence.operands()[1], constant)) {
      return constant;
    }
    if (recurrence.operands()[1] == phi &&
        detail::parseIntegerLiteral(recurrence.operands()[0], constant)) {
      return constant;
    }
    return std::nullopt;
  }

  if (recurrence.opcode() == "sub" && recurrence.operands()[0] == phi &&
      detail::parseIntegerLiteral(recurrence.operands()[1], constant)) {
    return -constant;
  }
  return std::nullopt;
}

std::vector<Induction> findInductions(const Function& function, const NaturalLoop& loop) {
  std::vector<Induction> inductions;
  if (!loop.preheader.has_value() || loop.header >= function.blocks().size() ||
      loop.latch >= function.blocks().size()) {
    return inductions;
  }

  const auto& headerName = function.blocks()[loop.header].name();
  const auto& preheaderName = function.blocks()[*loop.preheader].name();
  const auto& latchName = function.blocks()[loop.latch].name();
  const auto definitions = collectDefinitions(function);

  (void)headerName;
  for (const auto& instruction : function.blocks()[loop.header].instructions()) {
    if (instruction.opcode() != "phi" || !instruction.hasResult()) {
      break;
    }
    const auto& operands = instruction.operands();
    if (operands.empty() || operands.front() == "float" || operands.front() == "bool") {
      continue;
    }

    std::optional<std::string> init;
    std::optional<std::string> recurrenceValue;
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      if (operands[i + 1] == preheaderName) {
        init = operands[i];
      } else if (operands[i + 1] == latchName) {
        recurrenceValue = operands[i];
      }
    }
    if (!init.has_value() || !recurrenceValue.has_value()) {
      continue;
    }

    const auto recurrence = definitions.find(*recurrenceValue);
    if (recurrence == definitions.end()) {
      continue;
    }
    const auto step = matchRecurrenceStep(*recurrence->second, instruction.result());
    if (!step.has_value() || *step == 0) {
      continue;
    }
    inductions.push_back(Induction{instruction.result(), operands.front(), *init, *step});
  }
  return inductions;
}

std::optional<std::pair<std::string, long long>> matchInductionMul(
    const Instruction& instruction, const std::unordered_map<std::string, Induction>& inductions) {
  if (instruction.opcode() != "mul" || !instruction.hasResult() ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }

  long long factor = 0;
  if (inductions.find(instruction.operands()[0]) != inductions.end() &&
      detail::parseIntegerLiteral(instruction.operands()[1], factor)) {
    return std::make_pair(instruction.operands()[0], factor);
  }
  if (inductions.find(instruction.operands()[1]) != inductions.end() &&
      detail::parseIntegerLiteral(instruction.operands()[0], factor)) {
    return std::make_pair(instruction.operands()[1], factor);
  }
  return std::nullopt;
}

std::string scaledInitialValue(Function& function, const std::string& init, long long factor,
                               const std::string& type, std::size_t preheader,
                               std::unordered_set<std::string>& used) {
  if (factor == 0) {
    return "0";
  }
  if (factor == 1) {
    return init;
  }

  long long constant = 0;
  if (detail::parseIntegerLiteral(init, constant)) {
    return detail::formatIntegerLiteral(constant * factor);
  }

  auto& instructions = function.blocks()[preheader].instructions();
  auto insertPos = instructions.end();
  if (!instructions.empty() && instructions.back().isTerminator()) {
    insertPos = instructions.end() - 1;
  }

  const std::string result = freshName(function, "lsr.init", used);
  (void)type;
  instructions.insert(insertPos, Instruction{"mul",
                                             std::vector<std::string>{init,
                                                                      detail::formatIntegerLiteral(factor)},
                                             result});
  return result;
}

void insertHeaderPhi(Function& function, const NaturalLoop& loop, const Induction& induction,
                     long long factor, const LsrValue& value, const std::string& initValue) {
  const auto& preheaderName = function.blocks()[*loop.preheader].name();
  const auto& latchName = function.blocks()[loop.latch].name();
  auto& instructions = function.blocks()[loop.header].instructions();
  auto insertPos = instructions.begin();
  while (insertPos != instructions.end() && insertPos->opcode() == "phi") {
    ++insertPos;
  }

  (void)factor;
  instructions.insert(insertPos,
                      Instruction{"phi",
                                  std::vector<std::string>{induction.type, initValue,
                                                           preheaderName, value.next, latchName},
                                  value.phi});
}

void insertLatchStep(Function& function, const NaturalLoop& loop, long long stepProduct,
                     const LsrValue& value) {
  auto& instructions = function.blocks()[loop.latch].instructions();
  auto insertPos = instructions.end();
  if (!instructions.empty() && instructions.back().isTerminator()) {
    insertPos = instructions.end() - 1;
  }

  if (stepProduct == 0) {
    instructions.insert(insertPos, Instruction{"add", std::vector<std::string>{value.phi, "0"},
                                               value.next});
    return;
  }
  const std::string opcode = stepProduct >= 0 ? "add" : "sub";
  instructions.insert(insertPos,
                      Instruction{opcode,
                                  std::vector<std::string>{value.phi,
                                                           detail::formatIntegerLiteral(
                                                               stepProduct >= 0 ? stepProduct
                                                                                : -stepProduct)},
                                  value.next});
}

void rewriteFunctionOperands(Function& function,
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

bool runLoopStrengthReduction(Function& function) {
  const auto loops = compiler::midend::analysis::analyzeNaturalLoops(function);
  if (loops.empty()) {
    return false;
  }

  std::unordered_set<std::string> used;
  bool changed = false;
  for (const auto& loop : loops) {
    if (!loop.preheader.has_value()) {
      continue;
    }

    const auto inductions = findInductions(function, loop);
    if (inductions.empty()) {
      continue;
    }
    std::unordered_map<std::string, Induction> inductionByPhi;
    for (const auto& induction : inductions) {
      inductionByPhi.emplace(induction.phi, induction);
    }

    struct Use {
      std::string result;
      std::string phi;
      long long factor = 0;
    };
    std::vector<Use> uses;
    for (const auto blockIndex : loop.blocks) {
      if (blockIndex >= function.blocks().size()) {
        continue;
      }
      for (const auto& instruction : function.blocks()[blockIndex].instructions()) {
        const auto matched = matchInductionMul(instruction, inductionByPhi);
        if (!matched.has_value()) {
          continue;
        }
        if (matched->second == 0 || matched->second == 1) {
          continue;
        }
        uses.push_back(Use{instruction.result(), matched->first, matched->second});
      }
    }
    if (uses.empty()) {
      continue;
    }

    std::unordered_map<std::string, LsrValue> replacementByKey;
    std::unordered_map<std::string, std::string> aliases;
    for (const auto& use : uses) {
      const auto& induction = inductionByPhi.at(use.phi);
      const std::string key = induction.phi + "\n" + detail::formatIntegerLiteral(use.factor);
      auto value = replacementByKey.find(key);
      if (value == replacementByKey.end()) {
        const LsrValue lsr{freshName(function, "lsr.iv", used),
                           freshName(function, "lsr.next", used)};
        const std::string initValue =
            scaledInitialValue(function, induction.init, use.factor, induction.type,
                               *loop.preheader, used);
        insertHeaderPhi(function, loop, induction, use.factor, lsr, initValue);
        insertLatchStep(function, loop, induction.step * use.factor, lsr);
        value = replacementByKey.emplace(key, lsr).first;
        changed = true;
      }
      aliases[use.result] = value->second.phi;
    }

    rewriteFunctionOperands(function, aliases);
  }
  return changed;
}

}  // namespace

bool LoopStrengthReductionPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runLoopStrengthReduction(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
