#include "../../../include/compiler/midend/pass/ReassociatePass.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct Definition {
  std::string block;
  Instruction instruction;
};

struct MulFactor {
  std::string value;
  long long factor = 1;
};

bool isAssociativeOpcode(const std::string &opcode) {
  return opcode == "add" || opcode == "mul";
}

bool isLocalValue(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

std::string functionFingerprint(const Function &function) {
  std::ostringstream os;
  os << "func " << function.name() << ' ' << function.returnType() << '\n';
  for (const auto &block : function.blocks()) {
    os << "block " << block.name() << '\n';
    for (const auto &instruction : block.instructions()) {
      os << instruction.result() << " = " << instruction.opcode();
      for (const auto &operand : instruction.operands()) {
        os << ' ' << operand;
      }
      os << '\n';
    }
  }
  return os.str();
}

std::unordered_map<std::string, int> computeUseCounts(const Function &function) {
  std::unordered_map<std::string, int> uses;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      if (instruction.opcode() == "phi") {
        for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
          if (isLocalValue(operands[i])) {
            ++uses[operands[i]];
          }
        }
        continue;
      }
      if (instruction.opcode() == "call") {
        for (std::size_t i = 1; i < operands.size(); ++i) {
          if (isLocalValue(operands[i])) {
            ++uses[operands[i]];
          }
        }
        continue;
      }
      for (const auto &operand : operands) {
        if (isLocalValue(operand)) {
          ++uses[operand];
        }
      }
    }
  }
  return uses;
}

std::unordered_map<std::string, Definition>
computeDefinitions(const Function &function) {
  std::unordered_map<std::string, Definition> definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.insert_or_assign(instruction.result(),
                                     Definition{block.name(), instruction});
      }
    }
  }
  return definitions;
}

void collectTerms(const std::string &opcode, const std::string &blockName,
                  const std::string &operand,
                  const std::unordered_map<std::string, Definition> &definitions,
                  const std::unordered_map<std::string, int> &useCounts,
                  std::vector<std::string> &terms) {
  const auto defIt = definitions.find(operand);
  const auto useIt = useCounts.find(operand);
  if (defIt != definitions.end() && defIt->second.block == blockName &&
      defIt->second.instruction.opcode() == opcode &&
      defIt->second.instruction.operands().size() == 2 &&
      useIt != useCounts.end() && useIt->second == 1) {
    for (const auto &child : defIt->second.instruction.operands()) {
      collectTerms(opcode, blockName, child, definitions, useCounts, terms);
    }
    return;
  }
  terms.push_back(operand);
}

std::string nextTempName(const std::string &base, int &counter) {
  std::string stem = base.empty() ? "%reassoc" : base;
  if (!stem.empty() && stem.front() != '%') {
    stem.insert(stem.begin(), '%');
  }
  return stem + ".ra" + std::to_string(counter++);
}

std::vector<std::string> canonicalizeTerms(const std::string &opcode,
                                           std::vector<std::string> terms) {
  std::vector<std::string> values;
  values.reserve(terms.size() + 1);

  if (opcode == "add") {
    long long constant = 0;
    for (const auto &term : terms) {
      long long value = 0;
      if (detail::parseIntegerLiteral(term, value)) {
        constant += value;
      } else {
        values.push_back(term);
      }
    }
    std::sort(values.begin(), values.end());
    if (constant != 0 || values.empty()) {
      values.push_back(detail::formatIntegerLiteral(constant));
    }
    return values;
  }

  long long constant = 1;
  bool sawZero = false;
  for (const auto &term : terms) {
    long long value = 0;
    if (detail::parseIntegerLiteral(term, value)) {
      if (value == 0) {
        sawZero = true;
      } else {
        constant *= value;
      }
    } else {
      values.push_back(term);
    }
  }
  if (sawZero) {
    return {"0"};
  }
  std::sort(values.begin(), values.end());
  if (constant != 1 || values.empty()) {
    values.push_back(detail::formatIntegerLiteral(constant));
  }
  return values;
}

std::vector<std::string> normalizedOperandsForSingleTerm(
    const std::string &opcode, const std::string &term) {
  if (opcode == "add") {
    return {term, "0"};
  }
  return {term, "1"};
}

std::optional<MulFactor> parseMulByConstant(
    const std::string &operand, const std::string &blockName,
    const std::unordered_map<std::string, Definition> &definitions) {
  const auto defIt = definitions.find(operand);
  if (defIt == definitions.end() || defIt->second.block != blockName ||
      defIt->second.instruction.opcode() != "mul" ||
      defIt->second.instruction.operands().size() != 2) {
    return std::nullopt;
  }

  const auto &operands = defIt->second.instruction.operands();
  long long factor = 0;
  if (detail::parseIntegerLiteral(operands[0], factor)) {
    return MulFactor{operands[1], factor};
  }
  if (detail::parseIntegerLiteral(operands[1], factor)) {
    return MulFactor{operands[0], factor};
  }
  return std::nullopt;
}

std::optional<std::vector<Instruction>>
tryFactorCommonIntegerMultiplier(
    const Instruction &instruction, const std::string &blockName,
    const std::vector<std::string> &terms,
    const std::unordered_map<std::string, Definition> &definitions,
    int &tempCounter) {
  if (instruction.opcode() != "add" || terms.size() < 2) {
    return std::nullopt;
  }

  std::vector<std::string> factoredValues;
  factoredValues.reserve(terms.size());
  std::optional<long long> commonFactor;
  for (const auto &term : terms) {
    const auto factor = parseMulByConstant(term, blockName, definitions);
    if (!factor.has_value()) {
      return std::nullopt;
    }
    if (!commonFactor.has_value()) {
      commonFactor = factor->factor;
    } else if (*commonFactor != factor->factor) {
      return std::nullopt;
    }
    factoredValues.push_back(factor->value);
  }
  if (!commonFactor.has_value() || *commonFactor == 0 ||
      *commonFactor == 1 || *commonFactor == -1) {
    return std::nullopt;
  }

  std::vector<Instruction> built;
  const auto addTerms = canonicalizeTerms("add", std::move(factoredValues));
  std::string sumValue;
  if (addTerms.size() == 1) {
    sumValue = addTerms.front();
  } else {
    std::string accum = addTerms[0];
    for (std::size_t i = 1; i < addTerms.size(); ++i) {
      const bool last = i + 1 == addTerms.size();
      const std::string result =
          last ? nextTempName(instruction.result(), tempCounter)
               : nextTempName(instruction.result(), tempCounter);
      built.emplace_back("add", std::vector<std::string>{accum, addTerms[i]},
                         result);
      accum = result;
    }
    sumValue = accum;
  }

  built.emplace_back("mul",
                     std::vector<std::string>{
                         sumValue, detail::formatIntegerLiteral(*commonFactor)},
                     instruction.result());
  return built;
}

std::vector<Instruction> rebuildExpression(const Instruction &instruction,
                                           std::vector<std::string> terms,
                                           int &tempCounter) {
  terms = canonicalizeTerms(instruction.opcode(), std::move(terms));
  if (terms.size() == 1) {
    return {Instruction{instruction.opcode(),
                        normalizedOperandsForSingleTerm(instruction.opcode(),
                                                        terms.front()),
                        instruction.result()}};
  }

  std::vector<Instruction> built;
  std::string accum = terms[0];
  for (std::size_t i = 1; i < terms.size(); ++i) {
    const bool last = i + 1 == terms.size();
    const std::string result =
        last ? instruction.result() : nextTempName(instruction.result(), tempCounter);
    built.emplace_back(instruction.opcode(),
                       std::vector<std::string>{accum, terms[i]}, result);
    accum = result;
  }
  return built;
}

bool reassociateFunction(Function &function) {
  const auto before = functionFingerprint(function);
  auto definitions = computeDefinitions(function);
  const auto useCounts = computeUseCounts(function);
  int tempCounter = 0;

  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (!instruction.hasResult() || !isAssociativeOpcode(instruction.opcode()) ||
          instruction.operands().size() != 2) {
        rewritten.push_back(instruction);
        continue;
      }

      std::vector<std::string> terms;
      for (const auto &operand : instruction.operands()) {
        collectTerms(instruction.opcode(), block.name(), operand, definitions,
                     useCounts, terms);
      }

      if (auto factored =
              tryFactorCommonIntegerMultiplier(instruction, block.name(), terms,
                                               definitions, tempCounter);
          factored.has_value()) {
        rewritten.insert(rewritten.end(), factored->begin(), factored->end());
        continue;
      }

      auto rebuilt = rebuildExpression(instruction, std::move(terms), tempCounter);
      rewritten.insert(rewritten.end(), rebuilt.begin(), rebuilt.end());
    }
    block.instructions() = std::move(rewritten);
    definitions = computeDefinitions(function);
  }

  const auto after = functionFingerprint(function);
  return before != after;
}

} // namespace

bool ReassociatePass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    reassociateFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
