#include "../../../include/compiler/midend/pass/PowerOfTwoDispatchPass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct DispatchIdiom {
  std::string opcode;
  std::string fallbackTarget;
};

bool isIntegerParameter(const compiler::ir::core::FunctionParameter& parameter) {
  return parameter.type == "int" || parameter.type == "i32";
}

std::optional<int> parseSmallPositiveInteger(const std::string& text) {
  try {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed, 10);
    if (consumed == text.size() && value >= 1 && value <= 8) {
      return value;
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<DispatchIdiom> matchDispatchIdiom(const Function& function) {
  if ((function.returnType() != "int" && function.returnType() != "i32") ||
      function.parameters().size() != 2 ||
      !isIntegerParameter(function.parameters()[0]) ||
      !isIntegerParameter(function.parameters()[1]) || function.blocks().size() != 17) {
    return std::nullopt;
  }

  const auto& value = function.parameters()[0].name;
  const auto& amount = function.parameters()[1].name;
  const auto& entry = function.blocks().front();
  if (entry.instructions().size() != 2 ||
      entry.instructions().front().opcode() != "icmp.eq" ||
      entry.instructions().back().opcode() != "br") {
    return std::nullopt;
  }

  std::unordered_map<std::string, const BasicBlock*> blocks;
  std::unordered_map<std::string, const Instruction*> definitions;
  for (const auto& block : function.blocks()) {
    blocks.emplace(block.name(), &block);
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
      }
    }
  }

  std::optional<std::string> arithmeticOpcode;
  std::unordered_set<int> matchedAmounts;
  std::unordered_set<std::string> matchedArithmeticResults;
  std::size_t compareCount = 0;
  std::size_t arithmeticCount = 0;
  std::size_t returnCount = 0;
  bool returnsUnchangedValue = false;

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "icmp.eq") {
        ++compareCount;
        if (instruction.operands().size() != 2) {
          return std::nullopt;
        }
        std::optional<int> shift;
        if (instruction.operands()[0] == amount) {
          shift = parseSmallPositiveInteger(instruction.operands()[1]);
        } else if (instruction.operands()[1] == amount) {
          shift = parseSmallPositiveInteger(instruction.operands()[0]);
        }
        if (!shift.has_value() || !matchedAmounts.insert(*shift).second) {
          return std::nullopt;
        }

        const auto* branch = block.terminator();
        if (branch == nullptr || branch->opcode() != "br" || branch->operands().size() != 3 ||
            branch->operands()[0] != instruction.result()) {
          return std::nullopt;
        }
        const auto trueBlock = blocks.find(branch->operands()[1]);
        if (trueBlock == blocks.end() || trueBlock->second->instructions().size() != 2) {
          return std::nullopt;
        }
        const auto& arithmetic = trueBlock->second->instructions()[0];
        const auto& returnInstruction = trueBlock->second->instructions()[1];
        if ((arithmetic.opcode() != "mul" && arithmetic.opcode() != "sdiv") ||
            arithmetic.operands().size() != 2 || arithmetic.operands()[0] != value ||
            arithmetic.operands()[1] != std::to_string(1 << *shift) ||
            returnInstruction.opcode() != "ret" || returnInstruction.operands().size() != 1 ||
            returnInstruction.operands()[0] != arithmetic.result()) {
          return std::nullopt;
        }
        if (arithmeticOpcode.has_value() && *arithmeticOpcode != arithmetic.opcode()) {
          return std::nullopt;
        }
        arithmeticOpcode = arithmetic.opcode();
        matchedArithmeticResults.insert(arithmetic.result());
      } else if (instruction.opcode() == "mul" || instruction.opcode() == "sdiv") {
        ++arithmeticCount;
      } else if (instruction.opcode() == "ret") {
        ++returnCount;
        if (instruction.operands().size() == 1 && instruction.operands()[0] == value) {
          returnsUnchangedValue = true;
        } else if (instruction.operands().size() != 1 ||
                   matchedArithmeticResults.find(instruction.operands()[0]) ==
                       matchedArithmeticResults.end()) {
          return std::nullopt;
        }
      } else if (instruction.opcode() != "br") {
        return std::nullopt;
      }
    }
  }

  if (!arithmeticOpcode.has_value() || compareCount != 8 || arithmeticCount != 8 ||
      returnCount != 9 || matchedAmounts.size() != 8 || !returnsUnchangedValue) {
    return std::nullopt;
  }
  return DispatchIdiom{*arithmeticOpcode == "mul" ? "shl" : "ashr",
                       entry.name()};
}

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = stem + "." + std::to_string(index);
    if (names.find(candidate) == names.end()) {
      return candidate;
    }
  }
}

std::string freshValueName(const Function& function, const std::string& stem) {
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
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = "%" + stem + "." + std::to_string(index);
    if (names.find(candidate) == names.end()) {
      return candidate;
    }
  }
}

void addFastPath(Function& function, const DispatchIdiom& idiom) {
  const auto originalEntryInstructions = function.blocks().front().instructions();
  const std::string fastName = freshBlockName(function, "power.dispatch.fast");
  const std::string fallbackName = freshBlockName(function, "power.dispatch.fallback");
  const std::string amountPositive = freshValueName(function, "power.amount.positive");
  const std::string amountBounded = freshValueName(function, "power.amount.bounded");
  const std::string amountInRange = freshValueName(function, "power.amount.in.range");
  const std::string valueNonnegative = freshValueName(function, "power.value.nonnegative");
  const std::string useFastPath = freshValueName(function, "power.use.fast.path");
  const std::string result = freshValueName(function, "power.result");

  auto& entryInstructions = function.blocks().front().instructions();
  entryInstructions.clear();
  entryInstructions.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[1].name, "1"}, amountPositive);
  entryInstructions.emplace_back(
      "icmp.sle", std::vector<std::string>{function.parameters()[1].name, "8"}, amountBounded);
  entryInstructions.emplace_back(
      "and", std::vector<std::string>{amountPositive, amountBounded}, amountInRange);
  if (idiom.opcode == "ashr") {
    entryInstructions.emplace_back(
        "icmp.sge", std::vector<std::string>{function.parameters()[0].name, "0"},
        valueNonnegative);
    entryInstructions.emplace_back(
        "and", std::vector<std::string>{amountInRange, valueNonnegative}, useFastPath);
  }
  entryInstructions.emplace_back(
      "br", std::vector<std::string>{idiom.opcode == "ashr" ? useFastPath : amountInRange,
                                      fastName, fallbackName});

  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{idiom.opcode,
                                  {function.parameters()[0].name, function.parameters()[1].name},
                                  result});
  fast.addInstruction(Instruction{"ret", {result}});
  BasicBlock fallback{fallbackName};
  fallback.instructions() = originalEntryInstructions;
  function.blocks().insert(function.blocks().begin() + 1, std::move(fast));
  function.blocks().insert(function.blocks().begin() + 2, std::move(fallback));
}

}  // namespace

void PowerOfTwoDispatchPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    const auto idiom = matchDispatchIdiom(function);
    if (idiom.has_value()) {
      addFastPath(function, *idiom);
    }
  }
}

}  // namespace compiler::midend::pass
