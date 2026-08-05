#include "../../../include/compiler/midend/pass/RecursiveCallSpecializationPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"
#include "../../../include/compiler/midend/pass/ConstantFoldingPass.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

using DefinitionMap = std::unordered_map<std::string, const Instruction*>;

struct ReturnPredicate {
  std::string condition;
  bool returnWhenTrue = true;
};

struct EntryReturnSummary {
  std::vector<ReturnPredicate> predicates;
  std::size_t stateParameter = 0;
  std::int32_t terminalState = 0;
  std::string continuationBlock;
};

struct RecursiveTransition {
  std::size_t stateParameter = 0;
  std::int32_t step = 0;
};

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

const BasicBlock* findBlock(const Function& function, const std::string& name) {
  for (const auto& block : function.blocks()) {
    if (block.name() == name) return &block;
  }
  return nullptr;
}

bool isSafeNoEffectInstruction(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  return detail::isSideEffectFree(instruction) && opcode != "load" && opcode != "div" &&
         opcode != "mod" && opcode != "sdiv" && opcode != "srem";
}

bool isVoidReturnBlock(const Function& function, const std::string& name) {
  const auto* block = findBlock(function, name);
  if (block == nullptr || block->instructions().empty()) return false;
  for (std::size_t index = 0; index + 1 < block->instructions().size(); ++index) {
    if (!isSafeNoEffectInstruction(block->instructions()[index])) {
      return false;
    }
  }
  const auto& terminator = block->instructions().back();
  return terminator.opcode() == "ret" && terminator.operands().empty();
}

std::optional<std::int32_t> evaluateConstant(
    const std::string& value, const DefinitionMap& defs,
    const std::unordered_map<std::string, std::int32_t>& parameters,
    std::unordered_set<std::string>& visiting) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(literal));
  }
  const auto parameter = parameters.find(value);
  if (parameter != parameters.end()) return parameter->second;
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return std::nullopt;
  }
  const auto found = defs.find(value);
  if (found == defs.end()) {
    visiting.erase(value);
    return std::nullopt;
  }
  const auto& instruction = *found->second;
  const auto& operands = instruction.operands();
  auto unary = [&](const std::string& operand) {
    return evaluateConstant(operand, defs, parameters, visiting);
  };
  auto binary = [&]() -> std::optional<std::pair<std::int32_t, std::int32_t>> {
    if (operands.size() != 2) return std::nullopt;
    const auto lhs = unary(operands[0]);
    const auto rhs = unary(operands[1]);
    if (!lhs.has_value() || !rhs.has_value()) return std::nullopt;
    return std::pair<std::int32_t, std::int32_t>{*lhs, *rhs};
  };

  std::optional<std::int32_t> result;
  if ((instruction.opcode() == "add" || instruction.opcode() == "sub" ||
       instruction.opcode() == "and" || instruction.opcode() == "or" ||
       instruction.opcode() == "xor") &&
      operands.size() == 2) {
    const auto values = binary();
    if (values.has_value()) {
      const auto lhs = static_cast<std::uint32_t>(values->first);
      const auto rhs = static_cast<std::uint32_t>(values->second);
      std::uint32_t folded = 0;
      if (instruction.opcode() == "add") folded = lhs + rhs;
      if (instruction.opcode() == "sub") folded = lhs - rhs;
      if (instruction.opcode() == "and") folded = lhs & rhs;
      if (instruction.opcode() == "or") folded = lhs | rhs;
      if (instruction.opcode() == "xor") folded = lhs ^ rhs;
      result = static_cast<std::int32_t>(folded);
    }
  } else if (instruction.opcode() == "neg" && operands.size() == 1) {
    const auto operand = unary(operands[0]);
    if (operand.has_value()) {
      result = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(*operand));
    }
  } else if (instruction.opcode().rfind("icmp.", 0) == 0 && operands.size() == 2) {
    const auto values = binary();
    if (values.has_value()) {
      const auto lhs = values->first;
      const auto rhs = values->second;
      bool folded = false;
      const auto predicate = instruction.opcode().substr(5);
      if (predicate == "eq")
        folded = lhs == rhs;
      else if (predicate == "ne")
        folded = lhs != rhs;
      else if (predicate == "slt")
        folded = lhs < rhs;
      else if (predicate == "sle")
        folded = lhs <= rhs;
      else if (predicate == "sgt")
        folded = lhs > rhs;
      else if (predicate == "sge")
        folded = lhs >= rhs;
      else {
        const auto ulhs = static_cast<std::uint32_t>(lhs);
        const auto urhs = static_cast<std::uint32_t>(rhs);
        if (predicate == "ult")
          folded = ulhs < urhs;
        else if (predicate == "ule")
          folded = ulhs <= urhs;
        else if (predicate == "ugt")
          folded = ulhs > urhs;
        else if (predicate == "uge")
          folded = ulhs >= urhs;
        else {
          visiting.erase(value);
          return std::nullopt;
        }
      }
      result = folded ? 1 : 0;
    }
  } else if ((instruction.opcode() == "cast" || instruction.opcode() == "zext" ||
              instruction.opcode() == "sext" || instruction.opcode() == "trunc") &&
             operands.size() == 2) {
    result = unary(operands[0]);
  } else if (instruction.opcode() == "select" && operands.size() == 3) {
    const auto condition = unary(operands[0]);
    if (condition.has_value()) {
      result = unary(*condition != 0 ? operands[1] : operands[2]);
    }
  }
  visiting.erase(value);
  return result;
}

std::optional<std::int32_t> evaluateConstant(
    const std::string& value, const DefinitionMap& defs,
    const std::unordered_map<std::string, std::int32_t>& parameters = {}) {
  std::unordered_set<std::string> visiting;
  return evaluateConstant(value, defs, parameters, visiting);
}

bool dependsOnlyOnParameters(const std::string& value, const DefinitionMap& defs,
                             const std::unordered_set<std::string>& parameters,
                             std::unordered_set<std::string>& visiting) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal) || parameters.find(value) != parameters.end()) {
    return true;
  }
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return false;
  }
  const auto found = defs.find(value);
  if (found == defs.end()) {
    visiting.erase(value);
    return false;
  }
  const auto& instruction = *found->second;
  const auto& opcode = instruction.opcode();
  if (!detail::isSideEffectFree(instruction) || opcode == "alloca" || opcode == "load" ||
      opcode == "div" || opcode == "mod" || opcode == "sdiv" || opcode == "srem") {
    visiting.erase(value);
    return false;
  }
  std::size_t operandCount = instruction.operands().size();
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    operandCount = std::min<std::size_t>(1, operandCount);
  }
  for (std::size_t index = 0; index < operandCount; ++index) {
    if (!dependsOnlyOnParameters(instruction.operands()[index], defs, parameters, visiting)) {
      visiting.erase(value);
      return false;
    }
  }
  visiting.erase(value);
  return true;
}

std::optional<EntryReturnSummary> summarizeEntryReturn(const Function& function) {
  if (function.returnType() != "void" || function.blocks().empty()) {
    return std::nullopt;
  }
  const auto defs = definitions(function);
  std::unordered_set<std::string> parameterNames;
  for (const auto& parameter : function.parameters()) {
    parameterNames.insert(parameter.name);
  }

  EntryReturnSummary summary;
  const BasicBlock* current = &function.blocks().front();
  std::unordered_set<std::string> visited;
  for (std::size_t depth = 0; depth < 3; ++depth) {
    if (current == nullptr || !visited.insert(current->name()).second ||
        current->instructions().empty()) {
      break;
    }
    const auto& terminator = current->instructions().back();
    if (terminator.opcode() != "br" || terminator.operands().size() != 3) break;
    bool safePrefix = true;
    for (std::size_t index = 0; index + 1 < current->instructions().size(); ++index) {
      safePrefix &= isSafeNoEffectInstruction(current->instructions()[index]);
    }
    if (!safePrefix) break;
    std::unordered_set<std::string> visiting;
    if (!dependsOnlyOnParameters(terminator.operands()[0], defs, parameterNames, visiting)) {
      break;
    }
    const bool trueReturns = isVoidReturnBlock(function, terminator.operands()[1]);
    const bool falseReturns = isVoidReturnBlock(function, terminator.operands()[2]);
    if (trueReturns == falseReturns) break;
    summary.predicates.push_back(ReturnPredicate{terminator.operands()[0], trueReturns});
    current =
        findBlock(function, trueReturns ? terminator.operands()[2] : terminator.operands()[1]);
  }
  if (summary.predicates.empty()) return std::nullopt;

  for (std::size_t parameterIndex = 0; parameterIndex < function.parameters().size();
       ++parameterIndex) {
    const auto& parameter = function.parameters()[parameterIndex];
    if (parameter.type != "int") continue;
    for (const auto& predicate : summary.predicates) {
      const auto found = defs.find(predicate.condition);
      if (found == defs.end() || found->second->opcode() != "icmp.eq" ||
          found->second->operands().size() != 2 || !predicate.returnWhenTrue) {
        continue;
      }
      const auto& lhs = found->second->operands()[0];
      const auto& rhs = found->second->operands()[1];
      const std::string* other = nullptr;
      if (lhs == parameter.name) other = &rhs;
      if (rhs == parameter.name) other = &lhs;
      if (other == nullptr) continue;
      const auto terminal = evaluateConstant(*other, defs);
      if (!terminal.has_value()) continue;
      summary.stateParameter = parameterIndex;
      summary.terminalState = *terminal;
      if (current != nullptr) summary.continuationBlock = current->name();
      return summary;
    }
  }
  return std::nullopt;
}

bool isDirectSelfCall(const Instruction& instruction, const Function& function) {
  return instruction.opcode() == "call" && !instruction.operands().empty() &&
         instruction.operands()[0] == "@" + function.name() &&
         instruction.operands().size() == function.parameters().size() + 1;
}

std::optional<RecursiveTransition> recursiveTransition(const Function& function,
                                                       std::size_t stateParameter) {
  const auto defs = definitions(function);
  std::optional<std::int32_t> commonStep;
  bool sawCall = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (!isDirectSelfCall(instruction, function)) continue;
      sawCall = true;
      const auto& argument = instruction.operands()[stateParameter + 1];
      const auto found = defs.find(argument);
      if (found == defs.end() || found->second->operands().size() != 2) {
        return std::nullopt;
      }
      const auto& definition = *found->second;
      const auto& lhs = definition.operands()[0];
      const auto& rhs = definition.operands()[1];
      std::int32_t step = 0;
      if (definition.opcode() == "add") {
        if (lhs == function.parameters()[stateParameter].name) {
          const auto constant = evaluateConstant(rhs, defs);
          if (!constant.has_value()) return std::nullopt;
          step = *constant;
        } else if (rhs == function.parameters()[stateParameter].name) {
          const auto constant = evaluateConstant(lhs, defs);
          if (!constant.has_value()) return std::nullopt;
          step = *constant;
        } else {
          return std::nullopt;
        }
      } else if (definition.opcode() == "sub" &&
                 lhs == function.parameters()[stateParameter].name) {
        const auto constant = evaluateConstant(rhs, defs);
        if (!constant.has_value()) return std::nullopt;
        step = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(*constant));
      } else {
        return std::nullopt;
      }
      if (step == 0 || (commonStep.has_value() && *commonStep != step)) {
        return std::nullopt;
      }
      commonStep = step;
    }
  }
  if (!sawCall || !commonStep.has_value()) return std::nullopt;
  return RecursiveTransition{stateParameter, *commonStep};
}

std::size_t instructionCount(const Function& function) {
  std::size_t result = 0;
  for (const auto& block : function.blocks()) result += block.instructions().size();
  return result;
}

std::string freshFunctionName(const Module& module, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& function : module.functions()) names.insert(function.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

Function specializedClone(const Function& source, const std::string& name,
                          std::size_t stateParameter, std::int32_t specializedState) {
  Function clone{name, source.returnType(), source.parameters()};
  const auto stateName = source.parameters()[stateParameter].name;
  const auto literal = std::to_string(specializedState);
  for (const auto& block : source.blocks()) {
    BasicBlock copied{block.name()};
    for (const auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      for (auto& operand : operands) {
        if (operand == stateName) operand = literal;
      }
      copied.addInstruction(
          Instruction{instruction.opcode(), std::move(operands), instruction.result()});
    }
    clone.addBlock(std::move(copied));
  }
  return clone;
}

bool eraseTerminalCalls(Function& clone, const Function& original,
                        const EntryReturnSummary& summary) {
  const auto defs = definitions(clone);
  const auto stateName = clone.parameters()[summary.stateParameter].name;
  std::unordered_map<std::string, std::int32_t> specialized;
  // The clone substituted every use of this formal; keeping this map empty is
  // intentional because call arguments are resolved from the cloned defs.
  (void)stateName;
  bool erased = false;
  for (auto& block : clone.blocks()) {
    auto& instructions = block.instructions();
    instructions.erase(std::remove_if(instructions.begin(), instructions.end(),
                                      [&](const Instruction& instruction) {
                                        if (instruction.opcode() != "call" ||
                                            instruction.operands().empty() ||
                                            instruction.operands()[0] != "@" + original.name()) {
                                          return false;
                                        }
                                        const auto& stateArgument =
                                            instruction.operands()[summary.stateParameter + 1];
                                        const auto value = evaluateConstant(stateArgument, defs);
                                        if (!value.has_value() || *value != summary.terminalState) {
                                          return false;
                                        }
                                        erased = true;
                                        return true;
                                      }),
                       instructions.end());
  }
  return erased;
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

std::unordered_set<std::string> collectBlockNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) names.insert(block.name());
  return names;
}

std::string freshValue(std::unordered_set<std::string>& names, const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "%recursive.call." + stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) return candidate;
  }
}

std::string freshBlock(std::unordered_set<std::string>& names, const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "recursive.call." + stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) return candidate;
  }
}

std::vector<std::string> branchSuccessors(const std::vector<Instruction>& suffix) {
  if (suffix.empty()) return {};
  const auto& terminator = suffix.back();
  if (terminator.opcode() != "br") return {};
  if (terminator.operands().size() == 1) return {terminator.operands()[0]};
  if (terminator.operands().size() == 3) {
    return {terminator.operands()[1], terminator.operands()[2]};
  }
  return {};
}

void replacePhiPredecessor(Function& function, const std::vector<std::string>& successors,
                           const std::string& oldPredecessor, const std::string& newPredecessor) {
  const std::unordered_set<std::string> targets(successors.begin(), successors.end());
  for (auto& block : function.blocks()) {
    if (targets.find(block.name()) == targets.end()) continue;
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") continue;
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldPredecessor) operands[index] = newPredecessor;
      }
      instruction = Instruction{"phi", std::move(operands), instruction.result()};
    }
  }
}

std::string cloneExpression(const std::string& value, const DefinitionMap& calleeDefs,
                            const std::unordered_map<std::string, std::string>& parameters,
                            std::unordered_map<std::string, std::string>& cloned,
                            std::unordered_set<std::string>& valueNames,
                            std::vector<Instruction>& output) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) return value;
  const auto parameter = parameters.find(value);
  if (parameter != parameters.end()) return parameter->second;
  const auto prior = cloned.find(value);
  if (prior != cloned.end()) return prior->second;
  const auto found = calleeDefs.find(value);
  if (found == calleeDefs.end()) return {};
  const auto& source = *found->second;
  const auto& opcode = source.opcode();
  if (!detail::isSideEffectFree(source) || opcode == "alloca" || opcode == "load" ||
      opcode == "div" || opcode == "mod" || opcode == "sdiv" || opcode == "srem") {
    return {};
  }
  auto operands = source.operands();
  std::size_t operandCount = operands.size();
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    operandCount = std::min<std::size_t>(1, operandCount);
  }
  for (std::size_t index = 0; index < operandCount; ++index) {
    operands[index] =
        cloneExpression(operands[index], calleeDefs, parameters, cloned, valueNames, output);
    if (operands[index].empty()) return {};
  }
  const auto result = freshValue(valueNames, "guard");
  output.emplace_back(opcode, std::move(operands), result);
  cloned.emplace(value, result);
  return result;
}

bool specializeCallSite(Function& function, const Function& snapshot,
                        const EntryReturnSummary& summary, const std::string& specializedName,
                        std::int32_t specializedState, std::size_t blockIndex,
                        std::size_t instructionIndex) {
  const auto call = function.blocks()[blockIndex].instructions()[instructionIndex];
  if (!isDirectSelfCall(call, snapshot)) return false;
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto calleeDefs = definitions(snapshot);
  std::unordered_map<std::string, std::string> parameters;
  for (std::size_t index = 0; index < snapshot.parameters().size(); ++index) {
    parameters.emplace(snapshot.parameters()[index].name, call.operands()[index + 1]);
  }
  std::unordered_map<std::string, std::string> cloned;
  std::vector<Instruction> guardInstructions;
  std::string noOpCondition;
  for (const auto& predicate : summary.predicates) {
    auto condition = cloneExpression(predicate.condition, calleeDefs, parameters, cloned,
                                     valueNames, guardInstructions);
    if (condition.empty()) return false;
    if (!predicate.returnWhenTrue) {
      const auto inverted = freshValue(valueNames, "guard.not");
      guardInstructions.emplace_back("xor", std::vector<std::string>{condition, "1"}, inverted);
      condition = inverted;
    }
    if (noOpCondition.empty()) {
      noOpCondition = condition;
    } else {
      const auto combined = freshValue(valueNames, "guard.or");
      guardInstructions.emplace_back("or", std::vector<std::string>{noOpCondition, condition},
                                     combined);
      noOpCondition = combined;
    }
  }
  if (noOpCondition.empty()) return false;

  const auto stateArgument = call.operands()[summary.stateParameter + 1];
  const auto specializeCondition = freshValue(valueNames, "state");
  const auto dispatchName = freshBlock(blockNames, "dispatch");
  const auto genericName = freshBlock(blockNames, "generic");
  const auto specializedBlockName = freshBlock(blockNames, "specialized");
  const auto continuationName = freshBlock(blockNames, "continuation");
  const auto callerName = function.blocks()[blockIndex].name();

  auto& originalInstructions = function.blocks()[blockIndex].instructions();
  std::vector<Instruction> suffix(
      originalInstructions.begin() + static_cast<std::ptrdiff_t>(instructionIndex + 1),
      originalInstructions.end());
  const auto oldSuccessors = branchSuccessors(suffix);
  originalInstructions.erase(
      originalInstructions.begin() + static_cast<std::ptrdiff_t>(instructionIndex),
      originalInstructions.end());
  for (auto& instruction : guardInstructions) {
    originalInstructions.push_back(std::move(instruction));
  }
  originalInstructions.emplace_back(
      "br", std::vector<std::string>{noOpCondition, continuationName, dispatchName});

  BasicBlock dispatch{dispatchName};
  dispatch.addInstruction(Instruction{
      "icmp.eq", {stateArgument, std::to_string(specializedState)}, specializeCondition});
  dispatch.addInstruction(
      Instruction{"br", {specializeCondition, specializedBlockName, genericName}});

  BasicBlock generic{genericName};
  generic.addInstruction(call);
  generic.addInstruction(Instruction{"br", {continuationName}});

  auto specializedOperands = call.operands();
  specializedOperands[0] = "@" + specializedName;
  BasicBlock specialized{specializedBlockName};
  specialized.addInstruction(Instruction{"call", std::move(specializedOperands), call.result()});
  specialized.addInstruction(Instruction{"br", {continuationName}});

  BasicBlock continuation{continuationName};
  for (auto& instruction : suffix) continuation.addInstruction(std::move(instruction));
  replacePhiPredecessor(function, oldSuccessors, callerName, continuationName);

  auto insertion = function.blocks().begin() + static_cast<std::ptrdiff_t>(blockIndex + 1);
  insertion = function.blocks().insert(insertion, std::move(dispatch));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(specialized));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(generic));
  ++insertion;
  function.blocks().insert(insertion, std::move(continuation));
  return true;
}

struct Access {
  std::string root;
  std::string index;
};

struct UniformPartitionCandidate {
  std::string arrayParameter;
  std::string rangeBegin;
  std::string rangeEnd;
  std::string countAlloca;
  std::string headAlloca;
  std::string tailAlloca;
  std::size_t extent = 0;
  std::string selector;
  std::string selectorElement;
};

struct CountedRange {
  std::string begin;
  std::string end;
};

bool isGep(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  return opcode == "getelementptr" || opcode == "getelementptr.linear" || opcode == "gep" ||
         opcode == "gep.linear";
}

bool isStore(const Instruction& instruction) {
  return instruction.opcode() == "store" &&
         (instruction.operands().size() == 2 || instruction.operands().size() == 3);
}

const std::string& storeValue(const Instruction& instruction) {
  return instruction.operands()[instruction.operands().size() - 2];
}

const std::string& storeAddress(const Instruction& instruction) {
  return instruction.operands().back();
}

std::optional<Access> accessFor(const std::string& address, const DefinitionMap& defs) {
  const auto found = defs.find(address);
  if (found == defs.end() || !isGep(*found->second) || found->second->operands().size() != 2) {
    return std::nullopt;
  }
  return Access{found->second->operands()[0], found->second->operands()[1]};
}

std::optional<std::size_t> staticArrayExtent(const Instruction& instruction) {
  if (instruction.opcode() != "alloca" || instruction.operands().size() != 1) {
    return std::nullopt;
  }
  static const std::regex llvmPattern(R"(^\[([1-9][0-9]*) x .+\]$)");
  static const std::regex internalPattern(R"(^[^\[\]]+\[([1-9][0-9]*)\]$)");
  std::smatch match;
  if (!std::regex_match(instruction.operands()[0], match, llvmPattern) &&
      !std::regex_match(instruction.operands()[0], match, internalPattern)) {
    return std::nullopt;
  }
  try {
    const auto value = std::stoull(match[1].str());
    if (value < 2 || value > 256) return std::nullopt;
    return static_cast<std::size_t>(value);
  } catch (...) {
    return std::nullopt;
  }
}

bool allocaDoesNotEscape(const Function& function, const DefinitionMap& defs,
                         const std::string& alloca) {
  std::unordered_set<std::string> pointers{alloca};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [value, definition] : defs) {
      if (isGep(*definition) && definition->operands().size() == 2 &&
          pointers.find(definition->operands()[0]) != pointers.end()) {
        changed |= pointers.insert(value).second;
      }
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t index = 0; index < instruction.operands().size(); ++index) {
        if (pointers.find(instruction.operands()[index]) == pointers.end()) continue;
        if (isGep(instruction) && index == 0) continue;
        if (instruction.opcode() == "load" && index == 1) continue;
        if (isStore(instruction) && index + 1 == instruction.operands().size()) {
          continue;
        }
        return false;
      }
    }
  }
  return true;
}

bool isLoadFrom(const std::string& value, const std::string& address, const DefinitionMap& defs) {
  const auto found = defs.find(value);
  return found != defs.end() && found->second->opcode() == "load" &&
         found->second->operands().size() == 2 && found->second->operands()[1] == address;
}

bool loadsSameAddress(const std::string& lhs, const std::string& rhs, const DefinitionMap& defs) {
  if (lhs == rhs) return true;
  const auto lhsDefinition = defs.find(lhs);
  const auto rhsDefinition = defs.find(rhs);
  return lhsDefinition != defs.end() && rhsDefinition != defs.end() &&
         lhsDefinition->second->opcode() == "load" && rhsDefinition->second->opcode() == "load" &&
         lhsDefinition->second->operands().size() == 2 &&
         rhsDefinition->second->operands().size() == 2 &&
         lhsDefinition->second->operands()[1] == rhsDefinition->second->operands()[1];
}

bool isOneLess(const std::string& value, const std::string& reference, const DefinitionMap& defs) {
  const auto found = defs.find(value);
  if (found == defs.end() || found->second->opcode() != "sub" ||
      found->second->operands().size() != 2 || found->second->operands()[1] != "1") {
    return false;
  }
  return loadsSameAddress(found->second->operands()[0], reference, defs);
}

std::string pointerRoot(std::string address, const DefinitionMap& defs) {
  std::unordered_set<std::string> visited;
  while (visited.insert(address).second) {
    const auto found = defs.find(address);
    if (found == defs.end() || !isGep(*found->second) || found->second->operands().size() != 2) {
      break;
    }
    address = found->second->operands()[0];
  }
  return address;
}

bool isAddOneFromAddress(const std::string& value, const std::string& address,
                         const DefinitionMap& defs) {
  const auto found = defs.find(value);
  if (found == defs.end() || found->second->opcode() != "add" ||
      found->second->operands().size() != 2) {
    return false;
  }
  const auto& operands = found->second->operands();
  return (operands[0] == "1" && isLoadFrom(operands[1], address, defs)) ||
         (operands[1] == "1" && isLoadFrom(operands[0], address, defs));
}

bool isAddOne(const std::string& value, const std::string& prior, const DefinitionMap& defs) {
  const auto found = defs.find(value);
  if (found == defs.end() || found->second->opcode() != "add" ||
      found->second->operands().size() != 2) {
    return false;
  }
  const auto& operands = found->second->operands();
  return (operands[0] == prior && operands[1] == "1") ||
         (operands[1] == prior && operands[0] == "1");
}

void collectExpressionLeaves(const std::string& value, const DefinitionMap& defs,
                             std::unordered_set<std::string>& leaves,
                             std::unordered_set<std::string>& visiting) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) return;
  const auto found = defs.find(value);
  if (found == defs.end() || !visiting.insert(value).second) {
    leaves.insert(value);
    return;
  }
  const auto& instruction = *found->second;
  if (!detail::isSideEffectFree(instruction) || instruction.opcode() == "load" ||
      instruction.opcode() == "phi" || instruction.opcode() == "alloca" || isGep(instruction)) {
    leaves.insert(value);
    visiting.erase(value);
    return;
  }
  std::size_t count = instruction.operands().size();
  if (instruction.opcode() == "cast" || instruction.opcode() == "zext" ||
      instruction.opcode() == "sext" || instruction.opcode() == "trunc") {
    count = std::min<std::size_t>(1, count);
  }
  for (std::size_t index = 0; index < count; ++index) {
    collectExpressionLeaves(instruction.operands()[index], defs, leaves, visiting);
  }
  visiting.erase(value);
}

std::optional<std::string> expressionFingerprint(const std::string& value,
                                                 const DefinitionMap& defs,
                                                 const Function& function,
                                                 const std::string& element,
                                                 std::unordered_map<std::string, std::string>& memo,
                                                 std::unordered_set<std::string>& visiting) {
  if (value == element) return std::string{"$element"};
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) {
    return "#" + std::to_string(static_cast<std::int32_t>(static_cast<std::uint32_t>(literal)));
  }
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    if (function.parameters()[index].name == value) {
      return "$parameter" + std::to_string(index);
    }
  }
  const auto cached = memo.find(value);
  if (cached != memo.end()) return cached->second;
  const auto found = defs.find(value);
  if (found == defs.end() || !visiting.insert(value).second) return std::nullopt;
  const auto& instruction = *found->second;
  const auto& opcode = instruction.opcode();
  if (!detail::isSideEffectFree(instruction) || opcode == "load" || opcode == "phi" ||
      opcode == "alloca" || isGep(instruction)) {
    visiting.erase(value);
    return std::nullopt;
  }
  if ((opcode == "sdiv" || opcode == "srem" || opcode == "div" || opcode == "mod") &&
      (instruction.operands().size() != 2 ||
       !evaluateConstant(instruction.operands()[1], defs).has_value() ||
       *evaluateConstant(instruction.operands()[1], defs) == 0)) {
    visiting.erase(value);
    return std::nullopt;
  }
  std::size_t count = instruction.operands().size();
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    count = std::min<std::size_t>(1, count);
  }
  std::ostringstream stream;
  stream << opcode << '(';
  for (std::size_t index = 0; index < count; ++index) {
    const auto operand = expressionFingerprint(instruction.operands()[index], defs, function,
                                               element, memo, visiting);
    if (!operand.has_value()) {
      visiting.erase(value);
      return std::nullopt;
    }
    if (index != 0) stream << ',';
    stream << *operand;
  }
  stream << ')';
  visiting.erase(value);
  memo.emplace(value, stream.str());
  return stream.str();
}

std::optional<std::string> expressionFingerprint(const std::string& value,
                                                 const DefinitionMap& defs,
                                                 const Function& function,
                                                 const std::string& element) {
  std::unordered_map<std::string, std::string> memo;
  std::unordered_set<std::string> visiting;
  return expressionFingerprint(value, defs, function, element, memo, visiting);
}

bool hasCountedIndex(const Function& function, const DefinitionMap& defs, const std::string& index,
                     std::int32_t initial, std::size_t bound) {
  const auto load = defs.find(index);
  if (load == defs.end() || load->second->opcode() != "load" ||
      load->second->operands().size() != 2) {
    return false;
  }
  const auto& slot = load->second->operands()[1];
  bool initialized = false;
  bool incremented = false;
  bool bounded = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (isStore(instruction) && storeAddress(instruction) == slot) {
        const auto constant = evaluateConstant(storeValue(instruction), defs);
        initialized |= constant.has_value() && *constant == initial;
        incremented |= isAddOneFromAddress(storeValue(instruction), slot, defs);
      }
      if (instruction.opcode() == "icmp.slt" && instruction.operands().size() == 2) {
        const auto& operands = instruction.operands();
        bounded |= isLoadFrom(operands[0], slot, defs) &&
                   evaluateConstant(operands[1], defs) ==
                       std::optional<std::int32_t>{static_cast<std::int32_t>(bound)};
      }
    }
  }
  return initialized && incremented && bounded;
}

std::optional<CountedRange> countedRangeForBodyIndex(const Function& function,
                                                     const DefinitionMap& defs,
                                                     const BasicBlock& body,
                                                     const std::string& index) {
  const auto indexLoad = defs.find(index);
  if (indexLoad == defs.end() || indexLoad->second->opcode() != "load" ||
      indexLoad->second->operands().size() != 2) {
    return std::nullopt;
  }
  const auto& slot = indexLoad->second->operands()[1];
  bool increments = false;
  for (const auto& instruction : body.instructions()) {
    if (isStore(instruction) && storeAddress(instruction) == slot &&
        isAddOne(storeValue(instruction), index, defs)) {
      increments = true;
    }
  }
  const auto* bodyTerminator = body.terminator();
  if (!increments || bodyTerminator == nullptr || bodyTerminator->opcode() != "br" ||
      bodyTerminator->operands().size() != 1) {
    return std::nullopt;
  }
  const auto* condition = findBlock(function, bodyTerminator->operands()[0]);
  if (condition == nullptr || condition->terminator() == nullptr ||
      condition->terminator()->opcode() != "br" ||
      condition->terminator()->operands().size() != 3 ||
      condition->terminator()->operands()[1] != body.name()) {
    return std::nullopt;
  }
  const auto compare = defs.find(condition->terminator()->operands()[0]);
  if (compare == defs.end() || compare->second->opcode() != "icmp.slt" ||
      compare->second->operands().size() != 2 ||
      !isLoadFrom(compare->second->operands()[0], slot, defs)) {
    return std::nullopt;
  }
  const auto& end = compare->second->operands()[1];

  for (const auto& predecessor : function.blocks()) {
    if (predecessor.name() == body.name() || predecessor.terminator() == nullptr ||
        predecessor.terminator()->opcode() != "br" ||
        predecessor.terminator()->operands().size() != 1 ||
        predecessor.terminator()->operands()[0] != condition->name()) {
      continue;
    }
    for (auto instruction = predecessor.instructions().rbegin();
         instruction != predecessor.instructions().rend(); ++instruction) {
      if (isStore(*instruction) && storeAddress(*instruction) == slot) {
        return CountedRange{storeValue(*instruction), end};
      }
    }
  }
  return std::nullopt;
}

bool verifyPrefixBoundaries(const Function& function, const DefinitionMap& defs,
                            const std::string& countAlloca, const std::string& headAlloca,
                            const std::string& rangeBegin, std::size_t extent,
                            std::string& tailAlloca) {
  bool headZero = false;
  bool tailZero = false;
  bool recursiveStep = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (!isStore(instruction)) {
        continue;
      }
      const auto target = accessFor(storeAddress(instruction), defs);
      if (!target.has_value()) continue;
      const auto indexConstant = evaluateConstant(target->index, defs);
      if (target->root == headAlloca && indexConstant == 0 &&
          storeValue(instruction) == rangeBegin) {
        headZero = true;
      }
      const auto value = defs.find(storeValue(instruction));
      if (value == defs.end() || value->second->opcode() != "add" ||
          value->second->operands().size() != 2) {
        continue;
      }
      for (const auto& operand : value->second->operands()) {
        const auto load = defs.find(operand);
        if (load == defs.end() || load->second->opcode() != "load" ||
            load->second->operands().size() != 2) {
          continue;
        }
        const auto source = accessFor(load->second->operands()[1], defs);
        if (!source.has_value() || source->root != countAlloca ||
            !loadsSameAddress(source->index, target->index, defs)) {
          continue;
        }
        if (indexConstant == 0 && target->root != headAlloca) {
          const auto& other = value->second->operands()[0] == operand
                                  ? value->second->operands()[1]
                                  : value->second->operands()[0];
          if (other == rangeBegin) {
            tailAlloca = target->root;
            tailZero = true;
          }
        }
        if (!tailAlloca.empty() && target->root == tailAlloca && !indexConstant.has_value() &&
            hasCountedIndex(function, defs, target->index, 1, extent)) {
          const auto& other = value->second->operands()[0] == operand
                                  ? value->second->operands()[1]
                                  : value->second->operands()[0];
          const auto previousLoad = defs.find(other);
          if (previousLoad != defs.end() && previousLoad->second->opcode() == "load" &&
              previousLoad->second->operands().size() == 2) {
            const auto previous = accessFor(previousLoad->second->operands()[1], defs);
            if (previous.has_value() && previous->root == tailAlloca &&
                isOneLess(previous->index, target->index, defs)) {
              recursiveStep = true;
            }
          }
        }
      }
    }
  }
  return headZero && tailZero && recursiveStep && !tailAlloca.empty();
}

bool verifyCyclePartition(const Function& function, const DefinitionMap& defs,
                          const std::string& arrayParameter,
                          const std::string& histogramFingerprint, const std::string& rangeBegin,
                          std::size_t extent, std::string& headAlloca, std::string& tailAlloca,
                          const std::string& countAlloca) {
  for (const auto& conditionBlock : function.blocks()) {
    const auto* terminator = conditionBlock.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto compare = defs.find(terminator->operands()[0]);
    if (compare == defs.end() ||
        (compare->second->opcode() != "icmp.ne" && compare->second->opcode() != "icmp.eq") ||
        compare->second->operands().size() != 2) {
      continue;
    }
    for (int selectorSide = 0; selectorSide < 2; ++selectorSide) {
      const auto& selector = compare->second->operands()[selectorSide];
      const auto& outer = compare->second->operands()[1 - selectorSide];
      std::unordered_set<std::string> leaves;
      std::unordered_set<std::string> visiting;
      collectExpressionLeaves(selector, defs, leaves, visiting);
      for (const auto& currentValue : leaves) {
        const auto fingerprint = expressionFingerprint(selector, defs, function, currentValue);
        if (!fingerprint.has_value() || *fingerprint != histogramFingerprint) {
          continue;
        }
        const std::string bodyName = compare->second->opcode() == "icmp.ne"
                                         ? terminator->operands()[1]
                                         : terminator->operands()[2];
        const std::string exitName = compare->second->opcode() == "icmp.ne"
                                         ? terminator->operands()[2]
                                         : terminator->operands()[1];
        const auto* body = findBlock(function, bodyName);
        const auto* exit = findBlock(function, exitName);
        if (body == nullptr || exit == nullptr || body->terminator() == nullptr ||
            body->terminator()->opcode() != "br" || body->terminator()->operands().size() != 1 ||
            body->terminator()->operands()[0] != conditionBlock.name()) {
          continue;
        }
        std::string candidateHead;
        std::string displaced;
        bool bodyStore = false;
        bool bodyIncrement = false;
        for (const auto& instruction : body->instructions()) {
          if (!isStore(instruction)) {
            continue;
          }
          const auto target = accessFor(storeAddress(instruction), defs);
          if (!target.has_value()) continue;
          if (target->index == selector && target->root != arrayParameter) {
            if (isAddOneFromAddress(storeValue(instruction), storeAddress(instruction), defs)) {
              candidateHead = target->root;
              bodyIncrement = true;
            }
          }
          if (target->root == arrayParameter && storeValue(instruction) == currentValue) {
            const auto displacedLoad =
                std::find_if(body->instructions().begin(), body->instructions().end(),
                             [&](const Instruction& load) {
                               return load.opcode() == "load" && load.operands().size() == 2 &&
                                      load.operands()[1] == storeAddress(instruction);
                             });
            if (displacedLoad != body->instructions().end()) {
              displaced = displacedLoad->result();
              bodyStore = true;
            }
          }
        }
        if (!bodyStore || !bodyIncrement || candidateHead.empty()) continue;
        bool exitStore = false;
        bool exitIncrement = false;
        for (const auto& instruction : exit->instructions()) {
          if (!isStore(instruction)) {
            continue;
          }
          const auto target = accessFor(storeAddress(instruction), defs);
          if (!target.has_value()) continue;
          if (target->root == arrayParameter && storeValue(instruction) == currentValue) {
            exitStore = true;
          }
          if (target->root == candidateHead && loadsSameAddress(target->index, outer, defs)) {
            exitIncrement |=
                isAddOneFromAddress(storeValue(instruction), storeAddress(instruction), defs);
          }
        }
        const auto phi = defs.find(currentValue);
        bool phiCarriesDisplaced = false;
        if (phi != defs.end() && phi->second->opcode() == "phi") {
          for (std::size_t index = 1; index < phi->second->operands().size(); index += 2) {
            phiCarriesDisplaced |= phi->second->operands()[index] == displaced;
          }
        }
        if (!exitStore || !exitIncrement || !phiCarriesDisplaced) continue;
        if (!hasCountedIndex(function, defs, outer, 0, extent)) continue;
        std::string candidateTail;
        if (!verifyPrefixBoundaries(function, defs, countAlloca, candidateHead, rangeBegin, extent,
                                    candidateTail)) {
          continue;
        }
        bool headTailCondition = false;
        for (const auto& block : function.blocks()) {
          for (const auto& instruction : block.instructions()) {
            if (instruction.opcode() != "icmp.slt" || instruction.operands().size() != 2) {
              continue;
            }
            const auto lhsLoad = defs.find(instruction.operands()[0]);
            const auto rhsLoad = defs.find(instruction.operands()[1]);
            if (lhsLoad == defs.end() || rhsLoad == defs.end() ||
                lhsLoad->second->opcode() != "load" || rhsLoad->second->opcode() != "load" ||
                lhsLoad->second->operands().size() != 2 ||
                rhsLoad->second->operands().size() != 2) {
              continue;
            }
            const auto lhs = accessFor(lhsLoad->second->operands()[1], defs);
            const auto rhs = accessFor(rhsLoad->second->operands()[1], defs);
            headTailCondition |= lhs.has_value() && rhs.has_value() && lhs->root == candidateHead &&
                                 rhs->root == candidateTail &&
                                 loadsSameAddress(lhs->index, rhs->index, defs);
          }
        }
        if (!headTailCondition) continue;
        headAlloca = candidateHead;
        tailAlloca = candidateTail;
        return true;
      }
    }
  }
  return false;
}

std::optional<UniformPartitionCandidate> findUniformPartition(const Function& function) {
  const auto defs = definitions(function);
  std::unordered_map<std::string, std::size_t> localArrays;
  for (const auto& [value, definition] : defs) {
    const auto extent = staticArrayExtent(*definition);
    if (extent.has_value() && allocaDoesNotEscape(function, defs, value)) {
      localArrays.emplace(value, *extent);
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (!isStore(instruction)) {
        continue;
      }
      const auto count = accessFor(storeAddress(instruction), defs);
      if (!count.has_value()) continue;
      const auto extent = localArrays.find(count->root);
      if (extent == localArrays.end()) continue;
      const auto updated = defs.find(storeValue(instruction));
      if (updated == defs.end() || updated->second->opcode() != "add" ||
          updated->second->operands().size() != 2) {
        continue;
      }
      std::string countLoad;
      for (const auto& operand : updated->second->operands()) {
        if (isLoadFrom(operand, storeAddress(instruction), defs)) countLoad = operand;
      }
      if (countLoad.empty() || !isAddOne(storeValue(instruction), countLoad, defs)) {
        continue;
      }
      std::unordered_set<std::string> leaves;
      std::unordered_set<std::string> visiting;
      collectExpressionLeaves(count->index, defs, leaves, visiting);
      for (const auto& leaf : leaves) {
        const auto load = defs.find(leaf);
        if (load == defs.end() || load->second->opcode() != "load" ||
            load->second->operands().size() != 2) {
          continue;
        }
        const auto source = accessFor(load->second->operands()[1], defs);
        if (!source.has_value()) continue;
        bool arrayParameter = false;
        for (const auto& parameter : function.parameters()) {
          arrayParameter |= parameter.type != "int" && parameter.name == source->root;
        }
        if (!arrayParameter) continue;
        const auto range = countedRangeForBodyIndex(function, defs, block, source->index);
        if (!range.has_value()) continue;
        const auto fingerprint = expressionFingerprint(count->index, defs, function, leaf);
        if (!fingerprint.has_value()) continue;
        std::string head;
        std::string tail;
        if (!verifyCyclePartition(function, defs, source->root, *fingerprint, range->begin,
                                  extent->second, head, tail, count->root)) {
          continue;
        }
        return UniformPartitionCandidate{source->root,   range->begin, range->end,
                                         count->root,    head,         tail,
                                         extent->second, count->index, leaf};
      }
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> parameterIndex(const Function& function, const std::string& value) {
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    if (function.parameters()[index].name == value) return index;
  }
  return std::nullopt;
}

std::optional<Access> loadedAccess(const std::string& value, const DefinitionMap& defs) {
  const auto load = defs.find(value);
  if (load == defs.end() || load->second->opcode() != "load" ||
      load->second->operands().size() != 2) {
    return std::nullopt;
  }
  return accessFor(load->second->operands()[1], defs);
}

bool verifyUniformEffectsAndFanout(const Function& function,
                                   const UniformPartitionCandidate& candidate,
                                   const RecursiveTransition& transition) {
  const auto beginParameter = parameterIndex(function, candidate.rangeBegin);
  const auto endParameter = parameterIndex(function, candidate.rangeEnd);
  const auto arrayParameter = parameterIndex(function, candidate.arrayParameter);
  if (!beginParameter.has_value() || !endParameter.has_value() || !arrayParameter.has_value() ||
      *beginParameter == *endParameter || *arrayParameter == transition.stateParameter) {
    return false;
  }

  const auto defs = definitions(function);
  std::unordered_set<std::string> allocas;
  for (const auto& [value, definition] : defs) {
    if (definition->opcode() == "alloca") {
      if (!allocaDoesNotEscape(function, defs, value)) return false;
      allocas.insert(value);
    }
  }

  std::size_t selfCalls = 0;
  std::size_t arrayStores = 0;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "call") {
        if (!isDirectSelfCall(instruction, function)) return false;
        ++selfCalls;
        if (instruction.operands()[*arrayParameter + 1] != candidate.arrayParameter) {
          return false;
        }
        const auto begin = loadedAccess(instruction.operands()[*beginParameter + 1], defs);
        const auto end = loadedAccess(instruction.operands()[*endParameter + 1], defs);
        if (!begin.has_value() || !end.has_value() || begin->root != candidate.headAlloca ||
            end->root != candidate.tailAlloca ||
            !loadsSameAddress(begin->index, end->index, defs) ||
            !hasCountedIndex(function, defs, begin->index, 0, candidate.extent)) {
          return false;
        }
      }
      if (!isStore(instruction)) continue;
      const auto root = pointerRoot(storeAddress(instruction), defs);
      if (root == candidate.arrayParameter) {
        ++arrayStores;
      } else if (allocas.find(root) == allocas.end()) {
        return false;
      }
    }
  }
  return selfCalls == 1 && arrayStores == 2;
}

std::string cloneUniformExpression(const std::string& value, const DefinitionMap& defs,
                                   const std::string& element, const std::string& replacement,
                                   const Function& function,
                                   std::unordered_map<std::string, std::string>& cloned,
                                   std::unordered_set<std::string>& names,
                                   std::vector<Instruction>& output) {
  if (value == element) return replacement;
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) return value;
  for (const auto& parameter : function.parameters()) {
    if (parameter.name == value) return value;
  }
  const auto prior = cloned.find(value);
  if (prior != cloned.end()) return prior->second;
  const auto found = defs.find(value);
  if (found == defs.end()) return {};
  const auto& instruction = *found->second;
  const auto& opcode = instruction.opcode();
  if (!detail::isSideEffectFree(instruction) || opcode == "load" || opcode == "phi" ||
      opcode == "alloca" || isGep(instruction)) {
    return {};
  }
  if ((opcode == "sdiv" || opcode == "srem" || opcode == "div" || opcode == "mod") &&
      (instruction.operands().size() != 2 ||
       !evaluateConstant(instruction.operands()[1], defs).has_value() ||
       *evaluateConstant(instruction.operands()[1], defs) == 0)) {
    return {};
  }
  auto operands = instruction.operands();
  std::size_t count = operands.size();
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    count = std::min<std::size_t>(1, count);
  }
  for (std::size_t index = 0; index < count; ++index) {
    operands[index] = cloneUniformExpression(operands[index], defs, element, replacement, function,
                                             cloned, names, output);
    if (operands[index].empty()) return {};
  }
  const auto result = freshValue(names, "uniform.selector");
  output.emplace_back(opcode, std::move(operands), result);
  cloned.emplace(value, result);
  return result;
}

const Instruction* definitionWithOpcode(const std::string& value, const DefinitionMap& defs,
                                        const std::string& opcode, std::size_t operandCount) {
  const auto found = defs.find(value);
  if (found == defs.end() || found->second->opcode() != opcode ||
      found->second->operands().size() != operandCount) {
    return nullptr;
  }
  return found->second;
}

std::optional<unsigned> powerOfTwoDigitWidth(const Function& function,
                                             const UniformPartitionCandidate& candidate,
                                             const RecursiveTransition& transition) {
  if (transition.step != -1 || candidate.extent < 2 ||
      (candidate.extent & (candidate.extent - 1)) != 0) {
    return std::nullopt;
  }
  unsigned digitWidth = 0;
  for (std::size_t extent = candidate.extent; extent > 1; extent >>= 1) {
    ++digitWidth;
  }
  if (digitWidth == 0 || digitWidth >= 32) return std::nullopt;
  const auto threshold = static_cast<std::int32_t>((32U + digitWidth - 1) / digitWidth);
  const auto& state = function.parameters()[transition.stateParameter].name;
  const auto defs = definitions(function);

  const auto* result = definitionWithOpcode(candidate.selector, defs, "select", 3);
  if (result == nullptr || result->operands()[1] != "0") return std::nullopt;
  const auto* reachesZero = definitionWithOpcode(result->operands()[0], defs, "icmp.sge", 2);
  if (reachesZero == nullptr || reachesZero->operands()[0] != state ||
      evaluateConstant(reachesZero->operands()[1], defs) != threshold) {
    return std::nullopt;
  }

  const auto* remainder = definitionWithOpcode(result->operands()[2], defs, "srem", 2);
  if (remainder == nullptr || evaluateConstant(remainder->operands()[1], defs) !=
                                  static_cast<std::int32_t>(candidate.extent)) {
    return std::nullopt;
  }
  const auto* quotient = definitionWithOpcode(remainder->operands()[0], defs, "ashr", 2);
  if (quotient == nullptr) return std::nullopt;
  const auto& adjustedValue = quotient->operands()[0];
  const auto& shiftValue = quotient->operands()[1];

  const auto* shift = definitionWithOpcode(shiftValue, defs, "mul", 2);
  if (shift == nullptr) return std::nullopt;
  std::string selectedState;
  if (evaluateConstant(shift->operands()[0], defs) == static_cast<std::int32_t>(digitWidth)) {
    selectedState = shift->operands()[1];
  } else if (evaluateConstant(shift->operands()[1], defs) ==
             static_cast<std::int32_t>(digitWidth)) {
    selectedState = shift->operands()[0];
  } else {
    return std::nullopt;
  }
  const auto* selected = definitionWithOpcode(selectedState, defs, "select", 3);
  if (selected == nullptr || selected->operands()[1] != state || selected->operands()[2] != "0") {
    return std::nullopt;
  }
  const auto* inRange = definitionWithOpcode(selected->operands()[0], defs, "and", 2);
  if (inRange == nullptr) return std::nullopt;
  bool positive = false;
  bool belowThreshold = false;
  for (const auto& operand : inRange->operands()) {
    const auto* greater = definitionWithOpcode(operand, defs, "icmp.sgt", 2);
    positive |= greater != nullptr && greater->operands()[0] == state &&
                evaluateConstant(greater->operands()[1], defs) == 0;
    const auto* less = definitionWithOpcode(operand, defs, "icmp.slt", 2);
    belowThreshold |= less != nullptr && less->operands()[0] == state &&
                      evaluateConstant(less->operands()[1], defs) == threshold;
  }
  if (!positive || !belowThreshold) return std::nullopt;

  const auto* adjusted = definitionWithOpcode(adjustedValue, defs, "add", 2);
  if (adjusted == nullptr) return std::nullopt;
  std::string biasValue;
  if (adjusted->operands()[0] == candidate.selectorElement) {
    biasValue = adjusted->operands()[1];
  } else if (adjusted->operands()[1] == candidate.selectorElement) {
    biasValue = adjusted->operands()[0];
  } else {
    return std::nullopt;
  }
  const auto* bias = definitionWithOpcode(biasValue, defs, "and", 2);
  if (bias == nullptr) return std::nullopt;
  std::string signValue;
  std::string maskValue;
  for (const auto& operand : bias->operands()) {
    const auto* sign = definitionWithOpcode(operand, defs, "ashr", 2);
    if (sign != nullptr && sign->operands()[0] == candidate.selectorElement &&
        evaluateConstant(sign->operands()[1], defs) == 31) {
      signValue = operand;
    } else {
      maskValue = operand;
    }
  }
  if (signValue.empty() || maskValue.empty()) return std::nullopt;
  const auto* mask = definitionWithOpcode(maskValue, defs, "sub", 2);
  if (mask == nullptr || evaluateConstant(mask->operands()[1], defs) != 1) {
    return std::nullopt;
  }
  const auto* power = definitionWithOpcode(mask->operands()[0], defs, "shl", 2);
  if (power == nullptr || evaluateConstant(power->operands()[0], defs) != 1 ||
      power->operands()[1] != shiftValue) {
    return std::nullopt;
  }
  return digitWidth;
}

bool specializeExternalCommonPrefixCallSite(Function& caller, const Function& callee,
                                            const UniformPartitionCandidate& candidate,
                                            const RecursiveTransition& transition,
                                            unsigned digitWidth, const std::string& fastCalleeName,
                                            std::size_t blockIndex, std::size_t instructionIndex) {
  if (blockIndex >= caller.blocks().size() ||
      instructionIndex >= caller.blocks()[blockIndex].instructions().size()) {
    return false;
  }
  const auto call = caller.blocks()[blockIndex].instructions()[instructionIndex];
  if (call.opcode() != "call" || call.hasResult() || call.operands().empty() ||
      call.operands()[0] != "@" + callee.name() ||
      call.operands().size() != callee.parameters().size() + 1) {
    return false;
  }
  const auto beginParameter = parameterIndex(callee, candidate.rangeBegin);
  const auto endParameter = parameterIndex(callee, candidate.rangeEnd);
  const auto arrayParameter = parameterIndex(callee, candidate.arrayParameter);
  if (!beginParameter.has_value() || !endParameter.has_value() || !arrayParameter.has_value()) {
    return false;
  }
  const auto& begin = call.operands()[*beginParameter + 1];
  const auto& end = call.operands()[*endParameter + 1];
  const auto& array = call.operands()[*arrayParameter + 1];
  const auto& state = call.operands()[transition.stateParameter + 1];

  auto valueNames = collectValueNames(caller);
  auto blockNames = collectBlockNames(caller);
  const auto loadName = freshBlock(blockNames, "prefix.load");
  const auto headerName = freshBlock(blockNames, "prefix.header");
  const auto bodyName = freshBlock(blockNames, "prefix.body");
  const auto advanceName = freshBlock(blockNames, "prefix.advance");
  const auto summaryName = freshBlock(blockNames, "prefix.summary");
  const auto digitInitName = freshBlock(blockNames, "prefix.digit.init");
  const auto digitHeaderName = freshBlock(blockNames, "prefix.digit.header");
  const auto digitBodyName = freshBlock(blockNames, "prefix.digit.body");
  const auto dispatchName = freshBlock(blockNames, "prefix.dispatch");
  const auto fastCallName = freshBlock(blockNames, "prefix.fast");
  const auto fallbackName = freshBlock(blockNames, "prefix.fallback");
  const auto continuationName = freshBlock(blockNames, "prefix.continuation");

  const auto nonempty = freshValue(valueNames, "prefix.nonempty");
  const auto firstAddress = freshValue(valueNames, "prefix.first.address");
  const auto firstValue = freshValue(valueNames, "prefix.first.value");
  const auto firstNonnegative = freshValue(valueNames, "prefix.first.nonnegative");
  const auto firstIndex = freshValue(valueNames, "prefix.first.index");
  const auto index = freshValue(valueNames, "prefix.index");
  const auto difference = freshValue(valueNames, "prefix.difference");
  const auto inRange = freshValue(valueNames, "prefix.in.range");
  const auto address = freshValue(valueNames, "prefix.address");
  const auto value = freshValue(valueNames, "prefix.value");
  const auto nonnegative = freshValue(valueNames, "prefix.nonnegative");
  const auto changedBits = freshValue(valueNames, "prefix.changed.bits");
  const auto combined = freshValue(valueNames, "prefix.combined");
  const auto nextIndex = freshValue(valueNames, "prefix.next.index");
  const auto identical = freshValue(valueNames, "prefix.identical");
  const auto shifted = freshValue(valueNames, "prefix.shifted");
  const auto remaining = freshValue(valueNames, "prefix.remaining");
  const auto digit = freshValue(valueNames, "prefix.digit");
  const auto hasMore = freshValue(valueNames, "prefix.has.more");
  const auto nextRemaining = freshValue(valueNames, "prefix.next.remaining");
  const auto nextDigit = freshValue(valueNames, "prefix.next.digit");
  const auto canSkip = freshValue(valueNames, "prefix.can.skip");

  auto& original = caller.blocks()[blockIndex].instructions();
  std::vector<Instruction> suffix(
      original.begin() + static_cast<std::ptrdiff_t>(instructionIndex + 1), original.end());
  const auto oldSuccessors = branchSuccessors(suffix);
  const auto callerBlockName = caller.blocks()[blockIndex].name();
  original.erase(original.begin() + static_cast<std::ptrdiff_t>(instructionIndex), original.end());
  original.emplace_back("icmp.slt", std::vector<std::string>{begin, end}, nonempty);
  original.emplace_back("br", std::vector<std::string>{nonempty, loadName, fallbackName});

  BasicBlock load{loadName};
  load.addInstruction(Instruction{"getelementptr", {array, begin}, firstAddress});
  load.addInstruction(Instruction{"load", {"int", firstAddress}, firstValue});
  load.addInstruction(Instruction{"icmp.sge", {firstValue, "0"}, firstNonnegative});
  load.addInstruction(Instruction{"add", {begin, "1"}, firstIndex});
  load.addInstruction(Instruction{"br", {firstNonnegative, headerName, fallbackName}});

  BasicBlock header{headerName};
  header.addInstruction(
      Instruction{"phi", {"int", firstIndex, loadName, nextIndex, advanceName}, index});
  header.addInstruction(
      Instruction{"phi", {"int", "0", loadName, combined, advanceName}, difference});
  header.addInstruction(Instruction{"icmp.slt", {index, end}, inRange});
  header.addInstruction(Instruction{"br", {inRange, bodyName, summaryName}});

  BasicBlock body{bodyName};
  body.addInstruction(Instruction{"getelementptr", {array, index}, address});
  body.addInstruction(Instruction{"load", {"int", address}, value});
  body.addInstruction(Instruction{"icmp.sge", {value, "0"}, nonnegative});
  body.addInstruction(Instruction{"br", {nonnegative, advanceName, fallbackName}});

  BasicBlock advance{advanceName};
  advance.addInstruction(Instruction{"xor", {value, firstValue}, changedBits});
  advance.addInstruction(Instruction{"or", {difference, changedBits}, combined});
  advance.addInstruction(Instruction{"add", {index, "1"}, nextIndex});
  advance.addInstruction(Instruction{"br", {headerName}});

  BasicBlock summary{summaryName};
  summary.addInstruction(Instruction{"icmp.eq", {difference, "0"}, identical});
  summary.addInstruction(Instruction{"br", {identical, continuationName, digitInitName}});

  BasicBlock digitInit{digitInitName};
  digitInit.addInstruction(Instruction{"lshr", {difference, std::to_string(digitWidth)}, shifted});
  digitInit.addInstruction(Instruction{"br", {digitHeaderName}});

  BasicBlock digitHeader{digitHeaderName};
  digitHeader.addInstruction(
      Instruction{"phi", {"int", shifted, digitInitName, nextRemaining, digitBodyName}, remaining});
  digitHeader.addInstruction(
      Instruction{"phi", {"int", "0", digitInitName, nextDigit, digitBodyName}, digit});
  digitHeader.addInstruction(Instruction{"icmp.ne", {remaining, "0"}, hasMore});
  digitHeader.addInstruction(Instruction{"br", {hasMore, digitBodyName, dispatchName}});

  BasicBlock digitBody{digitBodyName};
  digitBody.addInstruction(
      Instruction{"lshr", {remaining, std::to_string(digitWidth)}, nextRemaining});
  digitBody.addInstruction(Instruction{"add", {digit, "1"}, nextDigit});
  digitBody.addInstruction(Instruction{"br", {digitHeaderName}});

  BasicBlock dispatch{dispatchName};
  dispatch.addInstruction(Instruction{"icmp.sgt", {state, digit}, canSkip});
  dispatch.addInstruction(Instruction{"br", {canSkip, fastCallName, fallbackName}});

  auto fastOperands = call.operands();
  fastOperands[0] = "@" + fastCalleeName;
  fastOperands[transition.stateParameter + 1] = digit;
  BasicBlock fastCall{fastCallName};
  fastCall.addInstruction(Instruction{"call", std::move(fastOperands)});
  fastCall.addInstruction(Instruction{"br", {continuationName}});

  BasicBlock fallback{fallbackName};
  fallback.addInstruction(call);
  fallback.addInstruction(Instruction{"br", {continuationName}});

  BasicBlock continuation{continuationName};
  for (auto& instruction : suffix) continuation.addInstruction(std::move(instruction));
  replacePhiPredecessor(caller, oldSuccessors, callerBlockName, continuationName);

  auto insertion = caller.blocks().begin() + static_cast<std::ptrdiff_t>(blockIndex + 1);
  insertion = caller.blocks().insert(insertion, std::move(load));
  insertion = caller.blocks().insert(++insertion, std::move(header));
  insertion = caller.blocks().insert(++insertion, std::move(body));
  insertion = caller.blocks().insert(++insertion, std::move(advance));
  insertion = caller.blocks().insert(++insertion, std::move(summary));
  insertion = caller.blocks().insert(++insertion, std::move(digitInit));
  insertion = caller.blocks().insert(++insertion, std::move(digitHeader));
  insertion = caller.blocks().insert(++insertion, std::move(digitBody));
  insertion = caller.blocks().insert(++insertion, std::move(dispatch));
  insertion = caller.blocks().insert(++insertion, std::move(fastCall));
  insertion = caller.blocks().insert(++insertion, std::move(fallback));
  caller.blocks().insert(++insertion, std::move(continuation));
  return true;
}

std::size_t specializeExternalCommonPrefixCalls(Module& module, std::size_t calleeIndex,
                                                const Function& callee,
                                                const UniformPartitionCandidate& candidate,
                                                const RecursiveTransition& transition,
                                                unsigned digitWidth,
                                                const std::string& fastCalleeName) {
  std::size_t transformed = 0;
  for (std::size_t functionIndex = 0; functionIndex < module.functions().size(); ++functionIndex) {
    if (functionIndex == calleeIndex) continue;
    auto& caller = module.functions()[functionIndex];
    std::vector<std::pair<std::size_t, std::size_t>> sites;
    for (std::size_t blockIndex = 0; blockIndex < caller.blocks().size(); ++blockIndex) {
      for (std::size_t instructionIndex = 0;
           instructionIndex < caller.blocks()[blockIndex].instructions().size();
           ++instructionIndex) {
        const auto& instruction = caller.blocks()[blockIndex].instructions()[instructionIndex];
        if (instruction.opcode() == "call" && !instruction.operands().empty() &&
            instruction.operands()[0] == "@" + callee.name()) {
          sites.emplace_back(blockIndex, instructionIndex);
        }
      }
    }
    for (auto site = sites.rbegin(); site != sites.rend(); ++site) {
      if (specializeExternalCommonPrefixCallSite(caller, callee, candidate, transition, digitWidth,
                                                 fastCalleeName, site->first, site->second)) {
        ++transformed;
      }
    }
  }
  return transformed;
}

Function renamedClone(const Function& source, const std::string& name) {
  Function clone{name, source.returnType(), source.parameters()};
  for (const auto& block : source.blocks()) {
    BasicBlock copied{block.name()};
    for (const auto& instruction : block.instructions()) {
      copied.addInstruction(
          Instruction{instruction.opcode(), instruction.operands(), instruction.result()});
    }
    clone.addBlock(std::move(copied));
  }
  return clone;
}

void rewriteDirectCallees(Function& function,
                          const std::unordered_map<std::string, std::string>& names) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "call" || instruction.operands().empty()) {
        continue;
      }
      const auto replacement = names.find(instruction.operands()[0]);
      if (replacement == names.end()) continue;
      auto operands = instruction.operands();
      operands[0] = replacement->second;
      instruction = Instruction{"call", std::move(operands), instruction.result()};
    }
  }
}

struct DigitSelectorSite {
  std::size_t blockIndex = 0;
  std::size_t instructionIndex = 0;
  std::string result;
  std::string element;
};

bool isArrayElementValue(const std::string& value, const std::string& arrayParameter,
                         const DefinitionMap& defs, std::unordered_set<std::string>& visiting) {
  const auto access = loadedAccess(value, defs);
  if (access.has_value()) return access->root == arrayParameter;
  const auto found = defs.find(value);
  if (found == defs.end() || found->second->opcode() != "phi" || !visiting.insert(value).second) {
    return false;
  }
  const auto& operands = found->second->operands();
  bool sawIncoming = false;
  bool valid = true;
  for (std::size_t index = 1; index < operands.size(); index += 2) {
    sawIncoming = true;
    valid &= isArrayElementValue(operands[index], arrayParameter, defs, visiting);
  }
  visiting.erase(value);
  return sawIncoming && valid;
}

bool rewriteNonnegativeDigitSelectors(Function& function, const std::string& arrayParameter,
                                      const std::string& selectorFingerprint,
                                      std::size_t stateParameter, unsigned digitWidth,
                                      std::size_t extent) {
  const auto defs = definitions(function);
  std::vector<DigitSelectorSite> sites;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size(); ++blockIndex) {
    const auto& block = function.blocks()[blockIndex];
    for (std::size_t instructionIndex = 0; instructionIndex < block.instructions().size();
         ++instructionIndex) {
      const auto& instruction = block.instructions()[instructionIndex];
      if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
        continue;
      }
      std::unordered_set<std::string> leaves;
      std::unordered_set<std::string> visiting;
      collectExpressionLeaves(instruction.result(), defs, leaves, visiting);
      for (const auto& leaf : leaves) {
        std::unordered_set<std::string> arrayVisiting;
        if (!isArrayElementValue(leaf, arrayParameter, defs, arrayVisiting)) {
          continue;
        }
        const auto fingerprint = expressionFingerprint(instruction.result(), defs, function, leaf);
        if (fingerprint.has_value() && *fingerprint == selectorFingerprint) {
          sites.push_back(
              DigitSelectorSite{blockIndex, instructionIndex, instruction.result(), leaf});
          break;
        }
      }
    }
  }
  if (sites.empty()) return false;

  auto names = collectValueNames(function);
  std::unordered_map<std::string, std::string> replacements;
  struct GeneratedSelector {
    DigitSelectorSite site;
    std::string shift;
    std::string shifted;
    std::string digit;
  };
  std::vector<GeneratedSelector> generated;
  for (const auto& site : sites) {
    const auto shift = freshValue(names, "nonnegative.shift");
    const auto shifted = freshValue(names, "nonnegative.shifted");
    const auto digit = freshValue(names, "nonnegative.digit");
    replacements.emplace(site.result, digit);
    generated.push_back(GeneratedSelector{site, shift, shifted, digit});
  }

  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      bool changed = false;
      for (auto& operand : operands) {
        const auto replacement = replacements.find(operand);
        if (replacement != replacements.end()) {
          operand = replacement->second;
          changed = true;
        }
      }
      if (changed) {
        instruction = Instruction{instruction.opcode(), std::move(operands), instruction.result()};
      }
    }
  }

  std::sort(generated.begin(), generated.end(),
            [](const GeneratedSelector& lhs, const GeneratedSelector& rhs) {
              if (lhs.site.blockIndex != rhs.site.blockIndex) {
                return lhs.site.blockIndex > rhs.site.blockIndex;
              }
              return lhs.site.instructionIndex > rhs.site.instructionIndex;
            });
  const auto& state = function.parameters()[stateParameter].name;
  for (auto& item : generated) {
    auto& instructions = function.blocks()[item.site.blockIndex].instructions();
    auto insertion = instructions.begin() + static_cast<std::ptrdiff_t>(item.site.instructionIndex);
    insertion = instructions.insert(
        insertion, Instruction{"mul", {state, std::to_string(digitWidth)}, item.shift});
    insertion = instructions.insert(
        ++insertion, Instruction{"lshr", {item.site.element, item.shift}, item.shifted});
    instructions.insert(++insertion,
                        Instruction{"and", {item.shifted, std::to_string(extent - 1)}, item.digit});
  }
  return true;
}

bool insertUniformPartitionBypass(Function& function, const EntryReturnSummary& summary,
                                  const RecursiveTransition& transition) {
  const auto candidate = findUniformPartition(function);
  if (!candidate.has_value() || summary.continuationBlock.empty() ||
      !verifyUniformEffectsAndFanout(function, *candidate, transition)) {
    return false;
  }
  auto target = std::find_if(
      function.blocks().begin(), function.blocks().end(),
      [&](const BasicBlock& block) { return block.name() == summary.continuationBlock; });
  if (target == function.blocks().end()) return false;
  for (const auto& instruction : target->instructions()) {
    if (instruction.opcode() == "phi") return false;
  }
  const auto defs = definitions(function);
  auto names = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto headerName = freshBlock(blockNames, "uniform.header");
  const auto bodyName = freshBlock(blockNames, "uniform.body");
  const auto advanceName = freshBlock(blockNames, "uniform.advance");
  const auto recurseName = freshBlock(blockNames, "uniform.recurse");
  const auto fallbackName = freshBlock(blockNames, "uniform.fallback");
  const auto firstAddress = freshValue(names, "uniform.first.address");
  const auto firstValue = freshValue(names, "uniform.first.value");
  const auto firstIndex = freshValue(names, "uniform.first.index");
  const auto index = freshValue(names, "uniform.index");
  const auto inRange = freshValue(names, "uniform.in.range");
  const auto address = freshValue(names, "uniform.address");
  const auto value = freshValue(names, "uniform.value");
  const auto same = freshValue(names, "uniform.same");
  const auto next = freshValue(names, "uniform.next");
  const auto nextState = freshValue(names, "uniform.next.state");

  std::unordered_map<std::string, std::string> clonedFirst;
  std::vector<Instruction> firstSelectorInstructions;
  const auto firstSelector =
      cloneUniformExpression(candidate->selector, defs, candidate->selectorElement, firstValue,
                             function, clonedFirst, names, firstSelectorInstructions);
  std::unordered_map<std::string, std::string> clonedCurrent;
  std::vector<Instruction> currentSelectorInstructions;
  const auto currentSelector =
      cloneUniformExpression(candidate->selector, defs, candidate->selectorElement, value, function,
                             clonedCurrent, names, currentSelectorInstructions);
  if (firstSelector.empty() || currentSelector.empty()) return false;

  std::vector<Instruction> fallbackInstructions = std::move(target->instructions());
  const auto oldSuccessors = branchSuccessors(fallbackInstructions);
  target->instructions().clear();
  target->addInstruction(Instruction{
      "getelementptr", {candidate->arrayParameter, candidate->rangeBegin}, firstAddress});
  target->addInstruction(Instruction{"load", {"int", firstAddress}, firstValue});
  for (auto& instruction : firstSelectorInstructions) {
    target->addInstruction(std::move(instruction));
  }
  target->addInstruction(Instruction{"add", {candidate->rangeBegin, "1"}, firstIndex});
  target->addInstruction(Instruction{"br", {headerName}});

  BasicBlock header{headerName};
  header.addInstruction(
      Instruction{"phi", {"int", firstIndex, target->name(), next, advanceName}, index});
  header.addInstruction(Instruction{"icmp.slt", {index, candidate->rangeEnd}, inRange});
  header.addInstruction(Instruction{"br", {inRange, bodyName, recurseName}});

  BasicBlock body{bodyName};
  body.addInstruction(Instruction{"getelementptr", {candidate->arrayParameter, index}, address});
  body.addInstruction(Instruction{"load", {"int", address}, value});
  for (auto& instruction : currentSelectorInstructions) {
    body.addInstruction(std::move(instruction));
  }
  body.addInstruction(Instruction{"icmp.eq", {currentSelector, firstSelector}, same});
  body.addInstruction(Instruction{"br", {same, advanceName, fallbackName}});

  BasicBlock advance{advanceName};
  advance.addInstruction(Instruction{"add", {index, "1"}, next});
  advance.addInstruction(Instruction{"br", {headerName}});

  BasicBlock recurse{recurseName};
  recurse.addInstruction(Instruction{
      "add",
      {function.parameters()[transition.stateParameter].name, std::to_string(transition.step)},
      nextState});
  std::vector<std::string> callOperands{"@" + function.name()};
  for (std::size_t parameter = 0; parameter < function.parameters().size(); ++parameter) {
    callOperands.push_back(
        parameter == transition.stateParameter ? nextState : function.parameters()[parameter].name);
  }
  recurse.addInstruction(Instruction{"call", std::move(callOperands)});
  recurse.addInstruction(Instruction{"ret", {}});

  BasicBlock fallback{fallbackName};
  for (auto& instruction : fallbackInstructions) {
    fallback.addInstruction(std::move(instruction));
  }
  replacePhiPredecessor(function, oldSuccessors, target->name(), fallbackName);

  auto insertion = target;
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(header));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(body));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(advance));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(recurse));
  ++insertion;
  function.blocks().insert(insertion, std::move(fallback));
  return true;
}

bool transformFunction(Module& module, std::size_t functionIndex) {
  if (functionIndex >= module.functions().size()) return false;
  const Function initialSnapshot = module.functions()[functionIndex];
  if (initialSnapshot.returnType() != "void" || initialSnapshot.parameters().empty() ||
      instructionCount(initialSnapshot) > 384) {
    return false;
  }
  const bool stats = std::getenv("COMPILER_RECURSIVE_CALL_SPECIALIZATION_STATS") != nullptr;
  const auto initialSummary = summarizeEntryReturn(initialSnapshot);
  if (!initialSummary.has_value()) return false;
  const auto initialTransition =
      recursiveTransition(initialSnapshot, initialSummary->stateParameter);
  if (!initialTransition.has_value()) return false;

  const auto initialPartition = findUniformPartition(initialSnapshot);
  std::optional<unsigned> commonPrefixDigitWidth;
  std::string selectorFingerprint;
  if (std::getenv("COMPILER_DISABLE_RECURSIVE_COMMON_PREFIX") == nullptr &&
      initialPartition.has_value() &&
      verifyUniformEffectsAndFanout(initialSnapshot, *initialPartition, *initialTransition)) {
    commonPrefixDigitWidth =
        powerOfTwoDigitWidth(initialSnapshot, *initialPartition, *initialTransition);
    if (commonPrefixDigitWidth.has_value()) {
      const auto defs = definitions(initialSnapshot);
      const auto fingerprint = expressionFingerprint(
          initialPartition->selector, defs, initialSnapshot, initialPartition->selectorElement);
      if (fingerprint.has_value()) {
        selectorFingerprint = *fingerprint;
      } else {
        commonPrefixDigitWidth.reset();
      }
    }
  }

  const bool uniformBypass = insertUniformPartitionBypass(module.functions()[functionIndex],
                                                          *initialSummary, *initialTransition);
  const Function snapshot = module.functions()[functionIndex];
  const auto summary = summarizeEntryReturn(snapshot);
  if (!summary.has_value()) return uniformBypass;
  const auto transition = recursiveTransition(snapshot, summary->stateParameter);
  if (!transition.has_value()) return uniformBypass;
  const auto predecessor =
      static_cast<std::int32_t>(static_cast<std::uint32_t>(summary->terminalState) -
                                static_cast<std::uint32_t>(transition->step));
  const auto specializedName = freshFunctionName(module, "__recursive_state_specialized_");
  auto clone = specializedClone(snapshot, specializedName, summary->stateParameter, predecessor);
  if (!eraseTerminalCalls(clone, snapshot, *summary)) {
    return uniformBypass;
  }
  module.addFunction(std::move(clone));

  bool changed = uniformBypass;
  std::size_t guardedSites = 0;
  auto& function = module.functions()[functionIndex];
  std::vector<std::pair<std::size_t, std::size_t>> sites;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size(); ++blockIndex) {
    for (std::size_t instructionIndex = 0;
         instructionIndex < function.blocks()[blockIndex].instructions().size();
         ++instructionIndex) {
      if (isDirectSelfCall(function.blocks()[blockIndex].instructions()[instructionIndex],
                           snapshot)) {
        sites.emplace_back(blockIndex, instructionIndex);
      }
    }
  }
  for (auto site = sites.rbegin(); site != sites.rend(); ++site) {
    if (!specializeCallSite(function, snapshot, *summary, specializedName, predecessor, site->first,
                            site->second)) {
      return changed;
    }
    changed = true;
    ++guardedSites;
  }
  std::size_t commonPrefixSites = 0;
  bool nonnegativeVersion = false;
  if (commonPrefixDigitWidth.has_value() && initialPartition.has_value()) {
    const auto nonnegativeName = freshFunctionName(module, "__recursive_nonnegative_");
    const auto nonnegativeTerminalName =
        freshFunctionName(module, "__recursive_nonnegative_terminal_");
    auto nonnegative = renamedClone(module.functions()[functionIndex], nonnegativeName);
    if (rewriteNonnegativeDigitSelectors(nonnegative, initialPartition->arrayParameter,
                                         selectorFingerprint, initialTransition->stateParameter,
                                         *commonPrefixDigitWidth, initialPartition->extent)) {
      rewriteDirectCallees(nonnegative, {{"@" + initialSnapshot.name(), "@" + nonnegativeName},
                                         {"@" + specializedName, "@" + nonnegativeTerminalName}});
      auto nonnegativeTerminal = specializedClone(nonnegative, nonnegativeTerminalName,
                                                  summary->stateParameter, predecessor);
      if (eraseTerminalCalls(nonnegativeTerminal, nonnegative, *summary)) {
        module.addFunction(std::move(nonnegative));
        module.addFunction(std::move(nonnegativeTerminal));
        commonPrefixSites = specializeExternalCommonPrefixCalls(
            module, functionIndex, initialSnapshot, *initialPartition, *initialTransition,
            *commonPrefixDigitWidth, nonnegativeName);
        nonnegativeVersion = commonPrefixSites != 0;
        changed |= nonnegativeVersion;
      }
    }
  }
  if (stats && changed) {
    std::cerr << "recursive-call-specialization: entry_guarded=" << guardedSites
              << " state_specialized=1 uniform_bypass=" << (uniformBypass ? 1 : 0)
              << " common_prefix_calls=" << commonPrefixSites
              << " nonnegative_version=" << (nonnegativeVersion ? 1 : 0) << '\n';
  }
  return changed;
}

}  // namespace

void RecursiveCallSpecializationPass::run(Module& module) {
  if (std::getenv("COMPILER_DISABLE_RECURSIVE_CALL_SPECIALIZATION") != nullptr) {
    return;
  }
  const std::size_t originalFunctions = module.functions().size();
  bool changed = false;
  for (std::size_t index = 0; index < originalFunctions; ++index) {
    changed |= transformFunction(module, index);
  }
  if (changed) {
    ConstantFoldingPass{true}.run(module);
    CommonSubexpressionEliminationPass{}.run(module);
  }
}

}  // namespace compiler::midend::pass
