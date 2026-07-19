#include "../../../include/compiler/ir/analysis/ScalarEvolutionAnalysis.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <unordered_map>

#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Module.h"

namespace compiler::ir::analysis {
namespace {

using compiler::ir::core::Instruction;

bool parseInteger(const std::string& text, long long& value) {
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoll(text.c_str(), &end, 0);
  if (errno == ERANGE || end == text.c_str() || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

std::string invertPredicate(const std::string& predicate) {
  if (predicate == "slt") return "sge";
  if (predicate == "sle") return "sgt";
  if (predicate == "sgt") return "sle";
  if (predicate == "sge") return "slt";
  if (predicate == "ult") return "uge";
  if (predicate == "ule") return "ugt";
  if (predicate == "ugt") return "ule";
  if (predicate == "uge") return "ult";
  if (predicate == "eq") return "ne";
  if (predicate == "ne") return "eq";
  return {};
}

std::string swapPredicate(const std::string& predicate) {
  if (predicate == "slt") return "sgt";
  if (predicate == "sle") return "sge";
  if (predicate == "sgt") return "slt";
  if (predicate == "sge") return "sle";
  if (predicate == "ult") return "ugt";
  if (predicate == "ule") return "uge";
  if (predicate == "ugt") return "ult";
  if (predicate == "uge") return "ule";
  return predicate;
}

std::optional<unsigned long long> exactTripCount(long long start,
                                                  long long bound,
                                                  long long step,
                                                  const std::string& predicate) {
  if (step == 0) return std::nullopt;
  unsigned long long distance = 0;
  unsigned long long divisor = 0;
  bool inclusive = false;
  if (step > 0 && (predicate == "slt" || predicate == "ult")) {
    if (start >= bound) return 0;
    distance = static_cast<unsigned long long>(bound) -
               static_cast<unsigned long long>(start);
    divisor = static_cast<unsigned long long>(step);
  } else if (step > 0 && (predicate == "sle" || predicate == "ule")) {
    if (start > bound) return 0;
    distance = static_cast<unsigned long long>(bound) -
               static_cast<unsigned long long>(start);
    divisor = static_cast<unsigned long long>(step);
    inclusive = true;
  } else if (step < 0 && (predicate == "sgt" || predicate == "ugt")) {
    if (start <= bound) return 0;
    distance = static_cast<unsigned long long>(start) -
               static_cast<unsigned long long>(bound);
    divisor = 0ULL - static_cast<unsigned long long>(step);
  } else if (step < 0 && (predicate == "sge" || predicate == "uge")) {
    if (start < bound) return 0;
    distance = static_cast<unsigned long long>(start) -
               static_cast<unsigned long long>(bound);
    divisor = 0ULL - static_cast<unsigned long long>(step);
    inclusive = true;
  } else {
    return std::nullopt;
  }
  if (inclusive) {
    if (distance == std::numeric_limits<unsigned long long>::max()) {
      return std::nullopt;
    }
    ++distance;
  }
  return distance == 0 ? 0 : 1 + (distance - 1) / divisor;
}

}  // namespace

ScalarEvolutionAnalysis::ScalarEvolutionAnalysis(
    const compiler::ir::core::Module& module,
    const compiler::ir::core::Function& function, const LoopAnalysis& loops) {
  (void)module;
  std::unordered_map<std::string, const Instruction*> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) definitions.emplace(instruction.result(), &instruction);
    }
  }

  for (const auto& loop : loops.loops()) {
    if (!loop.induction.has_value()) continue;
    const auto& induction = *loop.induction;
    recurrences_.emplace(
        induction.phi,
        ScalarAddRecurrence{induction.phi, induction.type,
                            induction.initialValue, induction.step, loop.header});

    std::string predicate = induction.predicate;
    std::string bound = induction.bound;
    const auto* terminator = function.blocks()[loop.header].terminator();
    if (terminator != nullptr && terminator->opcode() == "br" &&
        terminator->operands().size() == 3) {
      const auto compareFound = definitions.find(terminator->operands()[0]);
      if (compareFound != definitions.end() &&
          compareFound->second->operands().size() == 2) {
        const auto& compare = *compareFound->second;
        if (compare.operands()[1] == induction.phi) {
          predicate = swapPredicate(predicate);
        }
      }
      bool trueContinues = false;
      bool falseContinues = false;
      for (const auto block : loop.blocks) {
        trueContinues |= function.blocks()[block].name() == terminator->operands()[1];
        falseContinues |= function.blocks()[block].name() == terminator->operands()[2];
      }
      if (!trueContinues && falseContinues) predicate = invertPredicate(predicate);
      if (trueContinues == falseContinues) predicate.clear();
    }
    if (predicate.empty() || bound.empty()) continue;
    TripCount count{induction.initialValue, bound, induction.step, predicate,
                    std::nullopt};
    long long startValue = 0;
    long long boundValue = 0;
    if (parseInteger(count.start, startValue) &&
        parseInteger(count.bound, boundValue)) {
      count.exact = exactTripCount(startValue, boundValue, count.step,
                                   count.predicate);
    }
    tripCounts_.emplace(loop.header, std::move(count));
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult() || instruction.operands().size() != 2) continue;
        const auto& opcode = instruction.opcode();
        if (opcode != "getelementptr" && opcode != "getelementptr.linear" &&
            opcode != "gep" && opcode != "gep.linear") {
          continue;
        }
        const auto recurrenceFound = recurrences_.find(instruction.operands()[1]);
        if (recurrenceFound == recurrences_.end()) continue;
        const bool linear = opcode == "getelementptr.linear" || opcode == "gep.linear";
        AffineAddress address{instruction.result(), instruction.operands()[0],
                              instruction.operands()[1], recurrenceFound->second.start,
                              recurrenceFound->second.step,
                              recurrenceFound->second.loopHeader, linear};
        const auto inserted = addresses_.emplace(instruction.result(), std::move(address));
        changed |= inserted.second;
      }
    }
  }
}

std::optional<ScalarAddRecurrence> ScalarEvolutionAnalysis::recurrence(
    const std::string& value) const {
  const auto found = recurrences_.find(value);
  return found == recurrences_.end() ? std::nullopt
                                     : std::optional<ScalarAddRecurrence>{found->second};
}

std::optional<TripCount> ScalarEvolutionAnalysis::tripCount(
    std::size_t loopHeader) const {
  const auto found = tripCounts_.find(loopHeader);
  return found == tripCounts_.end() ? std::nullopt
                                    : std::optional<TripCount>{found->second};
}

std::optional<AffineAddress> ScalarEvolutionAnalysis::affineAddress(
    const std::string& value) const {
  const auto found = addresses_.find(value);
  return found == addresses_.end() ? std::nullopt
                                   : std::optional<AffineAddress>{found->second};
}

}  // namespace compiler::ir::analysis
