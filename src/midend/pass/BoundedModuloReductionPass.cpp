#include "../../../include/compiler/midend/pass/BoundedModuloReductionPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
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
using compiler::ir::core::BasicBlock;

enum class ReductionKind { Identity, Nonnegative };

struct Interval {
  std::int64_t lower = 0;
  std::int64_t upper = 0;
};

using FunctionRanges = std::unordered_map<std::string, Interval>;
using SpecializedFunctionRanges = std::unordered_map<std::string, Interval>;
using PredicateBounds = std::unordered_map<std::string, Interval>;

std::string specializedRangeKey(const std::string& function,
                                std::size_t parameter) {
  return function + "#nonnegative#" + std::to_string(parameter);
}

struct Candidate {
  std::size_t block = 0;
  std::size_t instruction = 0;
  ReductionKind kind = ReductionKind::Nonnegative;
  std::int32_t modulus = 0;
  std::string numerator;
  Instruction remainder{""};
};

std::optional<Interval> mergeIntervals(const std::optional<Interval>& lhs,
                                       const std::optional<Interval>& rhs) {
  if (!lhs.has_value()) return rhs;
  if (!rhs.has_value()) return lhs;
  return Interval{std::min(lhs->lower, rhs->lower),
                  std::max(lhs->upper, rhs->upper)};
}

bool isI32Interval(std::int64_t lower, std::int64_t upper) {
  return lower >= std::numeric_limits<std::int32_t>::min() &&
         upper <= std::numeric_limits<std::int32_t>::max();
}

std::unordered_map<std::string, const Instruction*> buildDefinitions(
    const Function& function) {
  std::unordered_map<std::string, const Instruction*> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
      }
    }
  }
  return definitions;
}

struct ControlFlowInfo {
  std::unordered_map<std::string, std::size_t> blockByName;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<std::unordered_set<std::size_t>> dominators;
};

ControlFlowInfo buildControlFlow(const Function& function) {
  ControlFlowInfo cfg;
  const std::size_t count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.dominators.resize(count);
  for (std::size_t block = 0; block < count; ++block) {
    cfg.blockByName.emplace(function.blocks()[block].name(), block);
  }
  for (std::size_t block = 0; block < count; ++block) {
    const auto* terminator = function.blocks()[block].terminator();
    if (terminator == nullptr || terminator->opcode() != "br") continue;
    const auto& operands = terminator->operands();
    const std::size_t firstTarget = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = firstTarget; index < operands.size(); ++index) {
      const auto target = cfg.blockByName.find(operands[index]);
      if (target == cfg.blockByName.end()) continue;
      cfg.successors[block].push_back(target->second);
      cfg.predecessors[target->second].push_back(block);
    }
  }
  std::unordered_set<std::size_t> all;
  for (std::size_t block = 0; block < count; ++block) all.insert(block);
  for (std::size_t block = 0; block < count; ++block) {
    cfg.dominators[block] = block == 0 ? std::unordered_set<std::size_t>{0}
                                        : all;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (cfg.predecessors[block].empty()) continue;
      auto next = cfg.dominators[cfg.predecessors[block].front()];
      for (std::size_t index = 1; index < cfg.predecessors[block].size();
           ++index) {
        std::unordered_set<std::size_t> intersection;
        for (const auto dominator : next) {
          if (cfg.dominators[cfg.predecessors[block][index]].count(dominator)) {
            intersection.insert(dominator);
          }
        }
        next = std::move(intersection);
      }
      next.insert(block);
      if (next != cfg.dominators[block]) {
        cfg.dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return cfg;
}

std::string swappedPredicate(const std::string& predicate) {
  if (predicate == "slt") return "sgt";
  if (predicate == "sle") return "sge";
  if (predicate == "sgt") return "slt";
  if (predicate == "sge") return "sle";
  return predicate;
}

void refinePredicateBound(PredicateBounds& bounds, const std::string& value,
                          const std::string& predicate, std::int64_t constant,
                          bool conditionTrue) {
  Interval next{std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()};
  std::string effective = predicate;
  if (!conditionTrue) {
    if (effective == "slt") effective = "sge";
    else if (effective == "sle") effective = "sgt";
    else if (effective == "sgt") effective = "sle";
    else if (effective == "sge") effective = "slt";
    else return;
  }
  if (effective == "slt") next.upper = constant - 1;
  else if (effective == "sle") next.upper = constant;
  else if (effective == "sgt") next.lower = constant + 1;
  else if (effective == "sge") next.lower = constant;
  else if (effective == "eq") next = Interval{constant, constant};
  else return;
  if (!isI32Interval(next.lower, next.upper)) return;
  const auto old = bounds.find(value);
  if (old != bounds.end()) {
    next.lower = std::max(next.lower, old->second.lower);
    next.upper = std::min(next.upper, old->second.upper);
  }
  if (next.lower <= next.upper) bounds[value] = next;
}

PredicateBounds predicateBoundsForBlock(
    const Function& function, std::size_t targetBlock,
    const std::unordered_map<std::string, const Instruction*>& definitions,
    const ControlFlowInfo& cfg) {
  PredicateBounds bounds;
  if (targetBlock >= cfg.dominators.size()) return bounds;
  for (const auto dominator : cfg.dominators[targetBlock]) {
    // A target dominated by exactly one successor of a conditional branch
    // inherits the corresponding signed comparison predicate.
    const auto* terminator = function.blocks()[dominator].terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto trueBlock = cfg.blockByName.find(terminator->operands()[1]);
    const auto falseBlock = cfg.blockByName.find(terminator->operands()[2]);
    if (trueBlock == cfg.blockByName.end() ||
        falseBlock == cfg.blockByName.end()) {
      continue;
    }
    const bool belowTrue =
        cfg.dominators[targetBlock].count(trueBlock->second) != 0;
    const bool belowFalse =
        cfg.dominators[targetBlock].count(falseBlock->second) != 0;
    if (belowTrue == belowFalse) continue;
    const auto compare = definitions.find(terminator->operands()[0]);
    if (compare == definitions.end() ||
        compare->second->opcode().rfind("icmp.", 0) != 0 ||
        compare->second->operands().size() != 2) {
      continue;
    }
    std::string value = compare->second->operands()[0];
    std::string predicate = compare->second->opcode().substr(5);
    long long constant = 0;
    if (!detail::parseIntegerLiteral(compare->second->operands()[1],
                                     constant)) {
      if (!detail::parseIntegerLiteral(compare->second->operands()[0],
                                       constant)) {
        continue;
      }
      value = compare->second->operands()[1];
      predicate = swappedPredicate(predicate);
    }
    refinePredicateBound(bounds, value, predicate, constant, belowTrue);
  }
  return bounds;
}

using SlotStores =
    std::unordered_map<std::string, std::vector<std::string>>;

SlotStores collectScalarSlotStores(const Function& function) {
  std::unordered_set<std::string> slots;
  std::unordered_set<std::string> escaped;
  SlotStores stores;
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
      for (std::size_t index = 0; index < instruction.operands().size();
           ++index) {
        const auto& operand = instruction.operands()[index];
        if (slots.find(operand) == slots.end()) continue;
        const bool loadAddress = instruction.opcode() == "load" && index == 1;
        const bool storeAddress =
            instruction.opcode() == "store" && index == 2;
        if (!loadAddress && !storeAddress) escaped.insert(operand);
      }
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3 ||
          slots.find(instruction.operands()[2]) == slots.end() ||
          escaped.find(instruction.operands()[2]) != escaped.end()) {
        continue;
      }
      stores[instruction.operands()[2]].push_back(instruction.operands()[1]);
    }
  }
  return stores;
}

std::optional<Interval> inferInterval(
    const std::string& value,
    const std::unordered_map<std::string, const Instruction*>& definitions,
    const SlotStores& slotStores, const FunctionRanges& functionRanges,
    const SpecializedFunctionRanges& specializedFunctionRanges,
    const std::unordered_set<std::string>& internalFunctions,
    const PredicateBounds& predicateBounds,
    std::unordered_map<std::string, std::optional<Interval>>& cache,
    std::unordered_set<std::string>& visiting,
    std::unordered_set<std::string>& bottomValues,
    std::size_t& unresolvedCalls, bool requireSpecializedCalls = false) {
  const std::size_t unresolvedAtEntry = unresolvedCalls;
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal) &&
      isI32Interval(literal, literal)) {
    return Interval{literal, literal};
  }
  if (bottomValues.find(value) != bottomValues.end()) {
    ++unresolvedCalls;
    return std::nullopt;
  }
  const auto cached = cache.find(value);
  if (cached != cache.end()) return cached->second;
  if (!visiting.insert(value).second) return std::nullopt;

  std::optional<Interval> result;
  const auto predicateBound = predicateBounds.find(value);
  const auto found = definitions.find(value);
  if (found != definitions.end()) {
    const Instruction& instruction = *found->second;
    const auto& operands = instruction.operands();
    auto operandRange = [&](std::size_t index) -> std::optional<Interval> {
      if (index >= operands.size()) return std::nullopt;
      return inferInterval(operands[index], definitions, slotStores,
                           functionRanges, specializedFunctionRanges,
                           internalFunctions, predicateBounds, cache, visiting,
                           bottomValues, unresolvedCalls,
                           requireSpecializedCalls);
    };
    if ((instruction.opcode() == "srem" || instruction.opcode() == "mod") &&
        operands.size() == 2) {
      long long modulus = 0;
      if (detail::parseIntegerLiteral(operands[1], modulus) && modulus > 0 &&
          modulus <= std::numeric_limits<std::int32_t>::max()) {
        const std::size_t unresolvedBefore = unresolvedCalls;
        const auto numerator = operandRange(0);
        if (!(requireSpecializedCalls && !numerator.has_value() &&
              unresolvedCalls != unresolvedBefore)) {
          result = Interval{-(modulus - 1), modulus - 1};
          if (numerator.has_value() && numerator->lower >= 0) {
            result->lower = 0;
          }
        }
      }
    } else if ((instruction.opcode() == "add" ||
                instruction.opcode() == "sub") &&
               operands.size() == 2) {
      const auto lhs = operandRange(0);
      const auto rhs = operandRange(1);
      if (lhs.has_value() && rhs.has_value()) {
        const std::int64_t lower =
            instruction.opcode() == "add" ? lhs->lower + rhs->lower
                                           : lhs->lower - rhs->upper;
        const std::int64_t upper =
            instruction.opcode() == "add" ? lhs->upper + rhs->upper
                                           : lhs->upper - rhs->lower;
        if (isI32Interval(lower, upper)) result = Interval{lower, upper};
      }
    } else if (instruction.opcode() == "select" && operands.size() == 3) {
      result = mergeIntervals(operandRange(1), operandRange(2));
    } else if (instruction.opcode() == "phi" && operands.size() >= 3) {
      std::optional<Interval> merged;
      bool complete = true;
      for (std::size_t index = 1; index < operands.size(); index += 2) {
        const std::size_t unresolvedBefore = unresolvedCalls;
        const auto incoming = operandRange(index);
        if (!incoming.has_value()) {
          if (unresolvedCalls != unresolvedBefore) continue;
          complete = false;
          break;
        }
        merged = mergeIntervals(merged, incoming);
      }
      if (complete) result = merged;
    } else if (instruction.opcode() == "call" && !operands.empty()) {
      bool expectedSpecialized = false;
      for (std::size_t argument = 1; argument < operands.size(); ++argument) {
        const auto argumentRange = operandRange(argument);
        if (!argumentRange.has_value() || argumentRange->lower < 0) continue;
        expectedSpecialized = true;
        const auto specialized = specializedFunctionRanges.find(
            specializedRangeKey(operands.front(), argument - 1));
        if (specialized == specializedFunctionRanges.end()) continue;
        if (!result.has_value()) {
          result = specialized->second;
        } else {
          result->lower = std::max(result->lower, specialized->second.lower);
          result->upper = std::min(result->upper, specialized->second.upper);
        }
      }
      if (!result.has_value() &&
          !(requireSpecializedCalls && expectedSpecialized)) {
        const auto summary = functionRanges.find(operands.front());
        if (summary != functionRanges.end()) result = summary->second;
      }
      if (!result.has_value() &&
          internalFunctions.find(operands.front()) != internalFunctions.end()) {
        if (requireSpecializedCalls && expectedSpecialized) {
          bottomValues.insert(value);
        }
        ++unresolvedCalls;
      }
    } else if (instruction.opcode() == "load" && operands.size() == 2) {
      const auto stores = slotStores.find(operands[1]);
      if (stores != slotStores.end() && !stores->second.empty()) {
        std::optional<Interval> merged;
        bool complete = true;
        for (const auto& stored : stores->second) {
          const std::size_t unresolvedBefore = unresolvedCalls;
          const auto storedRange = inferInterval(
              stored, definitions, slotStores, functionRanges,
              specializedFunctionRanges, internalFunctions, predicateBounds,
              cache, visiting, bottomValues, unresolvedCalls,
              requireSpecializedCalls);
          if (!storedRange.has_value()) {
            if (unresolvedCalls != unresolvedBefore) continue;
            complete = false;
            break;
          }
          merged = mergeIntervals(merged, storedRange);
        }
        if (complete) result = merged;
      }
    } else if (instruction.opcode() == "sext" && !operands.empty()) {
      result = operandRange(0);
    }
  }
  if (predicateBound != predicateBounds.end()) {
    if (!result.has_value()) {
      result = predicateBound->second;
    } else {
      result->lower = std::max(result->lower, predicateBound->second.lower);
      result->upper = std::min(result->upper, predicateBound->second.upper);
      if (result->lower > result->upper) result.reset();
    }
  }
  visiting.erase(value);
  if (result.has_value() || unresolvedCalls == unresolvedAtEntry) {
    cache[value] = result;
  }
  return result;
}

FunctionRanges computeFunctionRanges(const compiler::ir::core::Module& module) {
  FunctionRanges ranges;
  const SpecializedFunctionRanges noSpecializedRanges;
  std::unordered_set<std::string> internalFunctions;
  for (const auto& function : module.functions()) {
    internalFunctions.insert("@" + function.name());
  }
  constexpr std::size_t kMaximumIterations = 12;
  // Start from directly provable returns and grow recursive summaries to a
  // fixed point.  The validation phase below removes optimistic summaries
  // that still depend on an unresolved internal call.
  for (std::size_t iteration = 0; iteration < kMaximumIterations; ++iteration) {
    bool changed = false;
    for (const auto& function : module.functions()) {
      if (function.returnType() == "void") continue;
      const auto definitions = buildDefinitions(function);
      const auto slotStores = collectScalarSlotStores(function);
      std::unordered_map<std::string, std::optional<Interval>> cache;
      std::unordered_set<std::string> visiting;
      std::unordered_set<std::string> bottomValues;
      const PredicateBounds noPredicateBounds;
      std::size_t unresolvedCalls = 0;
      std::optional<Interval> returned;
      bool complete = true;
      for (const auto& block : function.blocks()) {
        for (const auto& instruction : block.instructions()) {
          if (instruction.opcode() != "ret" ||
              instruction.operands().size() != 1) {
            continue;
          }
          const std::size_t unresolvedBefore = unresolvedCalls;
          const auto value = inferInterval(instruction.operands()[0], definitions,
                                           slotStores, ranges,
                                           noSpecializedRanges,
                                           internalFunctions,
                                           noPredicateBounds, cache, visiting,
                                           bottomValues, unresolvedCalls);
          if (!value.has_value()) {
            if (unresolvedCalls != unresolvedBefore) continue;
            complete = false;
            break;
          }
          returned = mergeIntervals(returned, value);
        }
        if (!complete) break;
      }
      if (!complete || !returned.has_value()) continue;
      const std::string key = "@" + function.name();
      const auto old = ranges.find(key);
      if (old == ranges.end() || old->second.lower != returned->lower ||
          old->second.upper != returned->upper) {
        ranges[key] = *returned;
        changed = true;
      }
    }
    if (!changed) break;
  }
  bool removed = true;
  while (removed) {
    removed = false;
    for (const auto& function : module.functions()) {
      const std::string key = "@" + function.name();
      const auto summary = ranges.find(key);
      if (summary == ranges.end()) continue;
      const auto definitions = buildDefinitions(function);
      const auto slotStores = collectScalarSlotStores(function);
      const PredicateBounds noPredicateBounds;
      std::unordered_map<std::string, std::optional<Interval>> cache;
      std::unordered_set<std::string> visiting;
      std::unordered_set<std::string> bottomValues;
      std::size_t unresolvedCalls = 0;
      std::optional<Interval> returned;
      bool complete = true;
      for (const auto& block : function.blocks()) {
        for (const auto& instruction : block.instructions()) {
          if (instruction.opcode() != "ret" ||
              instruction.operands().size() != 1) {
            continue;
          }
          const auto value = inferInterval(
              instruction.operands()[0], definitions, slotStores, ranges,
              noSpecializedRanges, internalFunctions, noPredicateBounds, cache,
              visiting, bottomValues, unresolvedCalls);
          if (!value.has_value()) {
            complete = false;
            break;
          }
          returned = mergeIntervals(returned, value);
        }
        if (!complete) break;
      }
      if (!complete || unresolvedCalls != 0 || !returned.has_value() ||
          returned->lower != summary->second.lower ||
          returned->upper != summary->second.upper) {
        ranges.erase(key);
        removed = true;
        break;
      }
    }
  }
  return ranges;
}

enum class AbstractKind { Bottom, Known, Top };

struct AbstractValue {
  AbstractKind kind = AbstractKind::Bottom;
  Interval interval{};
};

bool sameAbstractValue(const AbstractValue& lhs, const AbstractValue& rhs) {
  return lhs.kind == rhs.kind &&
         (lhs.kind != AbstractKind::Known ||
          (lhs.interval.lower == rhs.interval.lower &&
           lhs.interval.upper == rhs.interval.upper));
}

AbstractValue joinAbstractValues(const AbstractValue& lhs,
                                 const AbstractValue& rhs) {
  if (lhs.kind == AbstractKind::Bottom) return rhs;
  if (rhs.kind == AbstractKind::Bottom) return lhs;
  if (lhs.kind == AbstractKind::Top || rhs.kind == AbstractKind::Top) {
    return AbstractValue{AbstractKind::Top, {}};
  }
  return AbstractValue{
      AbstractKind::Known,
      {std::min(lhs.interval.lower, rhs.interval.lower),
       std::max(lhs.interval.upper, rhs.interval.upper)}};
}

AbstractValue analyzeNonnegativeFunction(
    const Function& function, std::size_t nonnegativeParameter,
    const FunctionRanges& functionRanges,
    const SpecializedFunctionRanges& specializedFunctionRanges,
    bool requireSpecializedCalls,
    Interval parameterRange =
        {0, std::numeric_limits<std::int32_t>::max()},
    std::unordered_map<std::string, AbstractValue>* outputValues = nullptr) {
  std::unordered_map<std::string, AbstractValue> values;
  for (std::size_t parameter = 0; parameter < function.parameters().size();
       ++parameter) {
    values[function.parameters()[parameter].name] =
        parameter == nonnegativeParameter
            ? AbstractValue{AbstractKind::Known, parameterRange}
            : AbstractValue{AbstractKind::Top, {}};
  }
  const auto slotStores = collectScalarSlotStores(function);
  auto operandValue = [&](const std::string& operand) -> AbstractValue {
    long long literal = 0;
    if (detail::parseIntegerLiteral(operand, literal) &&
        isI32Interval(literal, literal)) {
      return AbstractValue{AbstractKind::Known, {literal, literal}};
    }
    const auto found = values.find(operand);
    return found == values.end() ? AbstractValue{} : found->second;
  };

  constexpr std::size_t kMaximumValueIterations = 32;
  for (std::size_t iteration = 0; iteration < kMaximumValueIterations;
       ++iteration) {
    bool changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult()) continue;
        const auto& operands = instruction.operands();
        AbstractValue next;
        if ((instruction.opcode() == "srem" ||
             instruction.opcode() == "mod") &&
            operands.size() == 2) {
          long long modulus = 0;
          if (detail::parseIntegerLiteral(operands[1], modulus) &&
              modulus > 0 &&
              modulus <= std::numeric_limits<std::int32_t>::max()) {
            const auto numerator = operandValue(operands[0]);
            if (numerator.kind == AbstractKind::Bottom) {
              next = numerator;
            } else {
              next = AbstractValue{AbstractKind::Known,
                                   {-(modulus - 1), modulus - 1}};
              if (numerator.kind == AbstractKind::Known &&
                  numerator.interval.lower >= 0) {
                next.interval.lower = 0;
              }
            }
          } else {
            next = AbstractValue{AbstractKind::Top, {}};
          }
        } else if ((instruction.opcode() == "add" ||
                    instruction.opcode() == "sub") &&
                   operands.size() == 2) {
          const auto lhs = operandValue(operands[0]);
          const auto rhs = operandValue(operands[1]);
          if (lhs.kind == AbstractKind::Bottom ||
              rhs.kind == AbstractKind::Bottom) {
            next = AbstractValue{};
          } else if (lhs.kind == AbstractKind::Top ||
                     rhs.kind == AbstractKind::Top) {
            next = AbstractValue{AbstractKind::Top, {}};
          } else {
            const std::int64_t lower =
                instruction.opcode() == "add"
                    ? lhs.interval.lower + rhs.interval.lower
                    : lhs.interval.lower - rhs.interval.upper;
            const std::int64_t upper =
                instruction.opcode() == "add"
                    ? lhs.interval.upper + rhs.interval.upper
                    : lhs.interval.upper - rhs.interval.lower;
            next = isI32Interval(lower, upper)
                       ? AbstractValue{AbstractKind::Known, {lower, upper}}
                       : AbstractValue{AbstractKind::Top, {}};
          }
        } else if (instruction.opcode() == "phi" && operands.size() >= 3) {
          for (std::size_t index = 1; index < operands.size(); index += 2) {
            next = joinAbstractValues(next, operandValue(operands[index]));
          }
        } else if (instruction.opcode() == "select" &&
                   operands.size() == 3) {
          next = joinAbstractValues(operandValue(operands[1]),
                                    operandValue(operands[2]));
        } else if (instruction.opcode() == "load" && operands.size() == 2) {
          const auto stores = slotStores.find(operands[1]);
          if (stores == slotStores.end() || stores->second.empty()) {
            next = AbstractValue{AbstractKind::Top, {}};
          } else {
            for (const auto& stored : stores->second) {
              next = joinAbstractValues(next, operandValue(stored));
            }
          }
        } else if (instruction.opcode() == "call" && !operands.empty()) {
          bool expectedSpecialized = false;
          for (std::size_t argument = 1; argument < operands.size();
               ++argument) {
            const auto argumentValue = operandValue(operands[argument]);
            if (argumentValue.kind != AbstractKind::Known ||
                argumentValue.interval.lower < 0) {
              continue;
            }
            expectedSpecialized = true;
            const auto specialized = specializedFunctionRanges.find(
                specializedRangeKey(operands.front(), argument - 1));
            if (specialized != specializedFunctionRanges.end()) {
              next = joinAbstractValues(
                  next, AbstractValue{AbstractKind::Known,
                                      specialized->second});
            }
          }
          if (next.kind == AbstractKind::Bottom &&
              !(requireSpecializedCalls && expectedSpecialized)) {
            const auto general = functionRanges.find(operands.front());
            if (general != functionRanges.end()) {
              next = AbstractValue{AbstractKind::Known, general->second};
            } else {
              next = AbstractValue{AbstractKind::Top, {}};
            }
          }
        } else if (instruction.opcode() == "sext" && !operands.empty()) {
          next = operandValue(operands[0]);
        } else {
          next = AbstractValue{AbstractKind::Top, {}};
        }
        const auto old = values.find(instruction.result());
        const AbstractValue merged =
            old == values.end() ? next : joinAbstractValues(old->second, next);
        if (old == values.end() || !sameAbstractValue(old->second, merged)) {
          values[instruction.result()] = merged;
          changed = true;
        }
      }
    }
    if (!changed) break;
  }

  AbstractValue returned;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "ret" && instruction.operands().size() == 1) {
        returned = joinAbstractValues(returned,
                                      operandValue(instruction.operands()[0]));
      }
    }
  }
  if (outputValues != nullptr) *outputValues = values;
  return returned;
}

SpecializedFunctionRanges computeNonnegativeFunctionRanges(
    const compiler::ir::core::Module& module,
    const FunctionRanges& functionRanges) {
  SpecializedFunctionRanges ranges;
  constexpr std::size_t kMaximumIterations = 12;
  for (std::size_t iteration = 0; iteration < kMaximumIterations; ++iteration) {
    bool changed = false;
    for (const auto& function : module.functions()) {
      if (function.returnType() == "void") continue;
      for (std::size_t parameter = 0; parameter < function.parameters().size();
           ++parameter) {
        const auto& currentParameter = function.parameters()[parameter];
        if (currentParameter.type != "int" && currentParameter.type != "i32") {
          continue;
        }
        const auto returned = analyzeNonnegativeFunction(
            function, parameter, functionRanges, ranges, true,
            {0, std::numeric_limits<std::int32_t>::max()});
        if (returned.kind != AbstractKind::Known) continue;
        const std::string key = specializedRangeKey(
            "@" + function.name(), parameter);
        const auto old = ranges.find(key);
        if (old == ranges.end() ||
            old->second.lower != returned.interval.lower ||
            old->second.upper != returned.interval.upper) {
          ranges[key] = returned.interval;
          changed = true;
        }
      }
    }
    if (!changed) break;
  }

  bool removed = true;
  while (removed) {
    removed = false;
    for (const auto& function : module.functions()) {
      for (std::size_t parameter = 0; parameter < function.parameters().size();
           ++parameter) {
        const std::string key = specializedRangeKey(
            "@" + function.name(), parameter);
        const auto summary = ranges.find(key);
        if (summary == ranges.end()) continue;
        const auto returned = analyzeNonnegativeFunction(
            function, parameter, functionRanges, ranges, false,
            {0, std::numeric_limits<std::int32_t>::max()});
        if (returned.kind != AbstractKind::Known ||
            returned.interval.lower != summary->second.lower ||
            returned.interval.upper != summary->second.upper) {
          ranges.erase(key);
          removed = true;
          break;
        }
      }
      if (removed) break;
    }
  }
  return ranges;
}

std::optional<std::int32_t> positiveSafeModulus(const std::string& operand) {
  long long value = 0;
  if (!detail::parseIntegerLiteral(operand, value) || value <= 1 ||
      value > (static_cast<long long>(
                   std::numeric_limits<std::int32_t>::max()) +
               1) /
                  2) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>(value);
}

std::optional<Candidate> findCandidate(
    const Function& function,
    const FunctionRanges& functionRanges,
    const SpecializedFunctionRanges& specializedFunctionRanges,
    const std::unordered_set<std::string>& internalFunctions,
    const std::unordered_set<std::string>& excludedResults) {
  const auto definitions = buildDefinitions(function);
  const auto slotStores = collectScalarSlotStores(function);
  const auto cfg = buildControlFlow(function);
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto& block = function.blocks()[blockIndex];
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions().size(); ++instructionIndex) {
      const auto& instruction = block.instructions()[instructionIndex];
      if ((instruction.opcode() != "srem" &&
           instruction.opcode() != "mod") ||
          !instruction.hasResult() || instruction.operands().size() != 2) {
        continue;
      }
      if (excludedResults.find(instruction.result()) != excludedResults.end()) {
        continue;
      }
      const auto modulus = positiveSafeModulus(instruction.operands()[1]);
      if (!modulus.has_value()) continue;
      const auto predicateBounds =
          predicateBoundsForBlock(function, blockIndex, definitions, cfg);
      std::unordered_map<std::string, std::optional<Interval>> cache;
      std::unordered_set<std::string> visiting;
      std::unordered_set<std::string> bottomValues;
      std::size_t unresolvedCalls = 0;
      const auto range = inferInterval(
          instruction.operands()[0], definitions, slotStores, functionRanges,
          specializedFunctionRanges, internalFunctions, predicateBounds, cache,
          visiting, bottomValues, unresolvedCalls);
      if (!range.has_value()) continue;

      const std::int64_t m = *modulus;
      ReductionKind kind;
      // For a positive M, signed remainder is already the identity on
      // (-M, M).  Values in [0, 2M) need one conditional subtraction.
      if (range->lower > -m && range->upper < m) {
        kind = ReductionKind::Identity;
      } else if (range->lower >= 0 && range->upper < 2 * m) {
        kind = ReductionKind::Nonnegative;
      } else {
        continue;
      }

      Candidate candidate;
      candidate.block = blockIndex;
      candidate.instruction = instructionIndex;
      candidate.kind = kind;
      candidate.modulus = *modulus;
      candidate.numerator = instruction.operands()[0];
      candidate.remainder = instruction;
      return candidate;
    }
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectValueNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  return names;
}

std::string freshName(std::unordered_set<std::string>& names,
                      const std::string& prefix) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = prefix + "." + std::to_string(suffix);
    if (names.insert(candidate).second) return candidate;
  }
}

void replaceUses(Function& function, const std::string& from,
                 const std::string& to) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      bool changed = false;
      for (auto& operand : operands) {
        if (operand == from) {
          operand = to;
          changed = true;
        }
      }
      if (changed) {
        instruction = Instruction{instruction.opcode(), std::move(operands),
                                  instruction.result()};
      }
    }
  }
}

void applyCandidate(Function& function, const Candidate& candidate) {
  auto& instructions = function.blocks()[candidate.block].instructions();
  auto position = instructions.begin() +
                  static_cast<std::ptrdiff_t>(candidate.instruction);
  if (candidate.kind == ReductionKind::Identity) {
    instructions.erase(position);
    replaceUses(function, candidate.remainder.result(), candidate.numerator);
    return;
  }

  auto valueNames = collectValueNames(function);
  const std::string modulus = std::to_string(candidate.modulus);
  const std::string highReduced =
      freshName(valueNames, "%bounded.mod.high.reduced");
  const std::string highCondition =
      freshName(valueNames, "%bounded.mod.high.condition");
  const std::string highValue = candidate.remainder.result();
  std::vector<Instruction> replacement;
  replacement.emplace_back(
      "sub", std::vector<std::string>{candidate.numerator, modulus},
      highReduced);
  replacement.emplace_back(
      "icmp.sge", std::vector<std::string>{candidate.numerator, modulus},
      highCondition);
  replacement.emplace_back(
      "select",
      std::vector<std::string>{highCondition, highReduced,
                               candidate.numerator},
      highValue);
  position = instructions.erase(position);
  instructions.insert(position, replacement.begin(), replacement.end());
}

struct GuardedRecurrenceCandidate {
  std::size_t block = 0;
  std::size_t instruction = 0;
  std::int32_t modulus = 0;
  std::string numerator;
  std::string remainder;
};

std::optional<GuardedRecurrenceCandidate>
findGuardedRecurrenceCandidate(const Function& function) {
  const auto definitions = buildDefinitions(function);
  std::unordered_map<std::string, std::size_t> blockByName;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    blockByName.emplace(function.blocks()[block].name(), block);
  }

  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto& block = function.blocks()[blockIndex];
    const auto* terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 1) {
      continue;
    }
    const auto successor = blockByName.find(terminator->operands()[0]);
    if (successor == blockByName.end()) continue;

    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions().size();
         ++instructionIndex) {
      const auto& instruction = block.instructions()[instructionIndex];
      if ((instruction.opcode() != "srem" &&
           instruction.opcode() != "mod") ||
          !instruction.hasResult() || instruction.operands().size() != 2) {
        continue;
      }
      const auto modulus = positiveSafeModulus(instruction.operands()[1]);
      if (!modulus.has_value()) continue;

      std::string recurrence;
      for (const auto& phi :
           function.blocks()[successor->second].instructions()) {
        if (phi.opcode() != "phi") continue;
        for (std::size_t incoming = 1;
             incoming + 1 < phi.operands().size(); incoming += 2) {
          if (phi.operands()[incoming] == instruction.result() &&
              phi.operands()[incoming + 1] == block.name()) {
            recurrence = phi.result();
          }
        }
      }
      if (recurrence.empty()) continue;

      const auto numerator = definitions.find(instruction.operands()[0]);
      if (numerator == definitions.end() ||
          numerator->second->opcode() != "add" ||
          numerator->second->operands().size() != 2) {
        continue;
      }
      const auto& addOperands = numerator->second->operands();
      std::string increment;
      if (addOperands[0] == recurrence) {
        increment = addOperands[1];
      } else if (addOperands[1] == recurrence) {
        increment = addOperands[0];
      } else {
        continue;
      }
      long long constantIncrement = 0;
      if (detail::parseIntegerLiteral(increment, constantIncrement)) {
        continue;
      }

      return GuardedRecurrenceCandidate{
          blockIndex, instructionIndex, *modulus,
          instruction.operands()[0], instruction.result()};
    }
  }
  return std::nullopt;
}

void rewritePhiPredecessor(Function& function,
                           const std::string& oldPredecessor,
                           const std::string& newPredecessor) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") continue;
      auto operands = instruction.operands();
      bool changed = false;
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldPredecessor) {
          operands[index] = newPredecessor;
          changed = true;
        }
      }
      if (changed) {
        instruction =
            Instruction{"phi", std::move(operands), instruction.result()};
      }
    }
  }
}

bool applyGuardedRecurrenceCandidate(
    Function& function, const GuardedRecurrenceCandidate& candidate) {
  if (candidate.block >= function.blocks().size()) return false;
  auto valueNames = collectValueNames(function);
  std::unordered_set<std::string> blockNames;
  for (const auto& block : function.blocks()) {
    blockNames.insert(block.name());
  }
  const std::string fastName =
      freshName(blockNames, "guarded.mod.fast");
  const std::string reduceName =
      freshName(blockNames, "guarded.mod.reduce");
  const std::string fallbackName =
      freshName(blockNames, "guarded.mod.fallback");
  const std::string continueName =
      freshName(blockNames, "guarded.mod.continue");
  const std::string alreadyReduced =
      freshName(valueNames, "%guarded.mod.already.reduced");
  const std::string reducibleOnce =
      freshName(valueNames, "%guarded.mod.reducible.once");
  const std::string reduced =
      freshName(valueNames, "%guarded.mod.reduced");
  const std::string fallbackValue =
      freshName(valueNames, "%guarded.mod.fallback.value");
  const std::string modulus = std::to_string(candidate.modulus);
  const std::string twiceModulus =
      std::to_string(static_cast<std::int64_t>(candidate.modulus) * 2);

  auto& original = function.blocks()[candidate.block];
  if (candidate.instruction >= original.instructions().size() ||
      original.instructions()[candidate.instruction].result() !=
          candidate.remainder) {
    return false;
  }
  const std::string originalName = original.name();
  std::vector<Instruction> prefix(
      original.instructions().begin(),
      original.instructions().begin() +
          static_cast<std::ptrdiff_t>(candidate.instruction));
  std::vector<Instruction> suffix(
      original.instructions().begin() +
          static_cast<std::ptrdiff_t>(candidate.instruction + 1),
      original.instructions().end());
  prefix.emplace_back(
      "icmp.ult",
      std::vector<std::string>{candidate.numerator, modulus},
      alreadyReduced);
  prefix.emplace_back(
      "br", std::vector<std::string>{alreadyReduced, continueName, fastName});
  original.instructions() = std::move(prefix);

  BasicBlock fast{fastName};
  fast.addInstruction(
      Instruction{"icmp.ult",
                  {candidate.numerator, twiceModulus},
                  reducibleOnce});
  fast.addInstruction(
      Instruction{"br", {reducibleOnce, reduceName, fallbackName}});

  BasicBlock reduce{reduceName};
  reduce.addInstruction(
      Instruction{"sub", {candidate.numerator, modulus}, reduced});
  reduce.addInstruction(Instruction{"br", {continueName}});

  BasicBlock fallback{fallbackName};
  fallback.addInstruction(Instruction{
      "srem", {candidate.numerator, modulus}, fallbackValue});
  fallback.addInstruction(Instruction{"br", {continueName}});

  BasicBlock continuation{continueName};
  continuation.addInstruction(Instruction{
      "phi",
      {"int", candidate.numerator, originalName, reduced, reduceName,
       fallbackValue, fallbackName},
      candidate.remainder});
  for (auto& instruction : suffix) {
    continuation.addInstruction(std::move(instruction));
  }

  rewritePhiPredecessor(function, originalName, continueName);
  function.addBlock(std::move(fast));
  function.addBlock(std::move(reduce));
  function.addBlock(std::move(fallback));
  function.addBlock(std::move(continuation));
  return true;
}

void rewriteDirectCalls(Function& function, const std::string& from,
                        const std::string& to) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "call" || instruction.operands().empty() ||
          instruction.operands().front() != from) {
        continue;
      }
      auto operands = instruction.operands();
      operands.front() = to;
      instruction = Instruction{"call", std::move(operands),
                                instruction.result()};
    }
  }
}

std::size_t instructionCount(const Function& function) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) count += block.instructions().size();
  return count;
}

bool hasDirectSelfCall(const Function& function) {
  const std::string symbol = "@" + function.name();
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "call" && !instruction.operands().empty() &&
          instruction.operands().front() == symbol) {
        return true;
      }
    }
  }
  return false;
}

std::vector<Candidate> candidatesFromAbstractValues(
    const Function& function,
    const std::unordered_map<std::string, AbstractValue>& values) {
  std::vector<Candidate> candidates;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto& instruction = instructions[index];
      if ((instruction.opcode() != "srem" && instruction.opcode() != "mod") ||
          !instruction.hasResult() || instruction.operands().size() != 2) {
        continue;
      }
      const auto modulus = positiveSafeModulus(instruction.operands()[1]);
      if (!modulus.has_value()) continue;
      const auto numerator = values.find(instruction.operands()[0]);
      if (numerator == values.end() ||
          numerator->second.kind != AbstractKind::Known) {
        continue;
      }
      const std::int64_t m = *modulus;
      ReductionKind kind;
      if (numerator->second.interval.lower > -m &&
          numerator->second.interval.upper < m) {
        kind = ReductionKind::Identity;
      } else if (numerator->second.interval.lower >= 0 &&
                 numerator->second.interval.upper < 2 * m) {
        kind = ReductionKind::Nonnegative;
      } else {
        continue;
      }
      candidates.push_back(Candidate{block, index, kind, *modulus,
                                     instruction.operands()[0], instruction});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) {
              if (lhs.block != rhs.block) return lhs.block > rhs.block;
              return lhs.instruction > rhs.instruction;
            });
  return candidates;
}

void versionOneBoundedRecursiveFunction(
    compiler::ir::core::Module& module, const FunctionRanges& functionRanges,
    const SpecializedFunctionRanges& specializedFunctionRanges) {
  constexpr std::size_t kMaximumVersionedInstructions = 256;
  const std::size_t originalFunctionCount = module.functions().size();
  std::unordered_set<std::string> functionNames;
  for (const auto& function : module.functions()) {
    functionNames.insert(function.name());
  }
  for (std::size_t functionIndex = 0; functionIndex < originalFunctionCount;
       ++functionIndex) {
    const Function& original = module.functions()[functionIndex];
    if (original.returnType() == "void" || !hasDirectSelfCall(original) ||
        instructionCount(original) > kMaximumVersionedInstructions) {
      continue;
    }
    const auto general = functionRanges.find("@" + original.name());
    if (general == functionRanges.end() || general->second.lower >= 0) continue;
    std::vector<std::int32_t> moduli;
    for (const auto& block : original.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if ((instruction.opcode() == "srem" || instruction.opcode() == "mod") &&
            instruction.operands().size() == 2) {
          const auto modulus = positiveSafeModulus(instruction.operands()[1]);
          if (modulus.has_value() &&
              std::find(moduli.begin(), moduli.end(), *modulus) ==
                  moduli.end()) {
            moduli.push_back(*modulus);
          }
        }
      }
    }
    std::sort(moduli.begin(), moduli.end(), std::greater<std::int32_t>());
    for (std::size_t parameter = 0; parameter < original.parameters().size();
         ++parameter) {
      if (original.parameters()[parameter].type != "int" &&
          original.parameters()[parameter].type != "i32") {
        continue;
      }
      for (const auto modulus : moduli) {
        SpecializedFunctionRanges boundedRanges = specializedFunctionRanges;
        const std::string boundedKey =
            specializedRangeKey("@" + original.name(), parameter);
        boundedRanges.erase(boundedKey);
        for (std::size_t iteration = 0; iteration < 12; ++iteration) {
          const auto returned = analyzeNonnegativeFunction(
              original, parameter, functionRanges, boundedRanges, true,
              {0, static_cast<std::int64_t>(modulus) - 1});
          if (returned.kind != AbstractKind::Known) break;
          const auto old = boundedRanges.find(boundedKey);
          if (old != boundedRanges.end() &&
              old->second.lower == returned.interval.lower &&
              old->second.upper == returned.interval.upper) {
            break;
          }
          boundedRanges[boundedKey] = returned.interval;
        }
        std::unordered_map<std::string, AbstractValue> values;
        const auto returned = analyzeNonnegativeFunction(
            original, parameter, functionRanges, boundedRanges, false,
            {0, static_cast<std::int64_t>(modulus) - 1}, &values);
        if (returned.kind != AbstractKind::Known ||
            returned.interval.lower < 0) {
          continue;
        }
        bool selfCallsPreserveBound = true;
        const std::string originalSymbol = "@" + original.name();
        for (const auto& block : original.blocks()) {
          for (const auto& instruction : block.instructions()) {
            if (instruction.opcode() != "call" ||
                instruction.operands().size() <= parameter + 1 ||
                instruction.operands().front() != originalSymbol) {
              continue;
            }
            const auto argument =
                values.find(instruction.operands()[parameter + 1]);
            if (argument == values.end() ||
                argument->second.kind != AbstractKind::Known ||
                argument->second.interval.lower < 0 ||
                argument->second.interval.upper >= modulus) {
              selfCallsPreserveBound = false;
            }
          }
        }
        if (!selfCallsPreserveBound) continue;
        auto candidates = candidatesFromAbstractValues(original, values);
        if (candidates.empty()) continue;

      const std::string fallbackName = freshName(functionNames,
                                                  original.name() + ".general");
      const std::string specializedName = freshName(
          functionNames, original.name() + ".bounded");
      Function fallback{fallbackName, original.returnType(),
                        original.parameters()};
      Function specializedFunction{specializedName, original.returnType(),
                                   original.parameters()};
      for (const auto& block : original.blocks()) {
        fallback.addBlock(block);
        specializedFunction.addBlock(block);
      }
      rewriteDirectCalls(fallback, "@" + original.name(), "@" + fallbackName);
      rewriteDirectCalls(specializedFunction, "@" + original.name(),
                         "@" + specializedName);
      for (const auto& candidate : candidates) {
        applyCandidate(specializedFunction, candidate);
      }

      Function& wrapper = module.functions()[functionIndex];
      wrapper.blocks().clear();
      std::unordered_set<std::string> wrapperValues;
      for (const auto& current : wrapper.parameters()) {
        wrapperValues.insert(current.name);
      }
      const std::string nonnegative =
          freshName(wrapperValues, "%bounded.mod.version.nonnegative");
      const std::string belowModulus =
          freshName(wrapperValues, "%bounded.mod.version.below");
      const std::string fastResult =
          freshName(wrapperValues, "%bounded.mod.version.fast");
      const std::string slowResult =
          freshName(wrapperValues, "%bounded.mod.version.slow");
      std::vector<std::string> fastCall{"@" + specializedName};
      std::vector<std::string> slowCall{"@" + fallbackName};
      for (const auto& current : wrapper.parameters()) {
        fastCall.push_back(current.name);
        slowCall.push_back(current.name);
      }
      BasicBlock entry{"entry"};
      entry.addInstruction(Instruction{
          "icmp.sge", {wrapper.parameters()[parameter].name, "0"}, nonnegative});
      entry.addInstruction(
          Instruction{"br", {nonnegative, "bound.check", "general"}});
      BasicBlock boundCheck{"bound.check"};
      boundCheck.addInstruction(Instruction{
          "icmp.slt",
          {wrapper.parameters()[parameter].name, std::to_string(modulus)},
          belowModulus});
      boundCheck.addInstruction(
          Instruction{"br", {belowModulus, "bounded", "general"}});
      BasicBlock fast{"bounded"};
      fast.addInstruction(Instruction{"call", std::move(fastCall), fastResult});
      fast.addInstruction(Instruction{"ret", {fastResult}});
      BasicBlock slow{"general"};
      slow.addInstruction(Instruction{"call", std::move(slowCall), slowResult});
      slow.addInstruction(Instruction{"ret", {slowResult}});
      wrapper.addBlock(std::move(entry));
      wrapper.addBlock(std::move(boundCheck));
      wrapper.addBlock(std::move(fast));
      wrapper.addBlock(std::move(slow));
      module.addFunction(std::move(fallback));
      module.addFunction(std::move(specializedFunction));
      return;
      }
    }
  }
}

}  // namespace

void BoundedModuloReductionPass::run(compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_BOUNDED_MODULO_REDUCTION") != nullptr) {
    return;
  }
  constexpr std::size_t kMaximumReductionsPerFunction = 16;
  const FunctionRanges initialFunctionRanges = computeFunctionRanges(module);
  const SpecializedFunctionRanges initialSpecializedFunctionRanges =
      computeNonnegativeFunctionRanges(module, initialFunctionRanges);
  versionOneBoundedRecursiveFunction(
      module, initialFunctionRanges, initialSpecializedFunctionRanges);
  const FunctionRanges functionRanges = computeFunctionRanges(module);
  const SpecializedFunctionRanges specializedFunctionRanges =
      computeNonnegativeFunctionRanges(module, functionRanges);
  std::unordered_set<std::string> internalFunctions;
  for (const auto& function : module.functions()) {
    internalFunctions.insert("@" + function.name());
  }
  for (auto& function : module.functions()) {
    std::unordered_set<std::string> excludedResults;
    std::vector<Candidate> candidates;
    for (std::size_t count = 0; count < kMaximumReductionsPerFunction;
         ++count) {
      const auto candidate =
          findCandidate(function, functionRanges, specializedFunctionRanges,
                        internalFunctions, excludedResults);
      if (!candidate.has_value()) break;
      excludedResults.insert(candidate->remainder.result());
      candidates.push_back(*candidate);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                if (lhs.block != rhs.block) return lhs.block > rhs.block;
                return lhs.instruction > rhs.instruction;
              });
    for (const auto& candidate : candidates) {
      applyCandidate(function, candidate);
    }
    if (std::getenv("COMPILER_DISABLE_GUARDED_MODULAR_RECURRENCE") ==
        nullptr) {
      const auto guarded = findGuardedRecurrenceCandidate(function);
      if (guarded.has_value()) {
        applyGuardedRecurrenceCandidate(function, *guarded);
      }
    }
  }
}

}  // namespace compiler::midend::pass
