#include "../../../include/compiler/midend/pass/PowerOfTwoDispatchPass.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
  std::string operation;
  std::string value;
  std::string shift;
  unsigned maximumShift = 0;
};

struct ParameterRange {
  bool seen = false;
  bool valid = true;
  unsigned lower = 0;
  unsigned upper = 0;
};

using ParameterRanges =
    std::unordered_map<std::string, std::vector<ParameterRange>>;

bool isI32(const std::string& type) { return type == "int" || type == "i32"; }

std::optional<unsigned> parsePositive(const std::string& text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return std::nullopt;
    value = value * 10U + static_cast<unsigned>(ch - '0');
    if (value > 0xffffffffULL) return std::nullopt;
  }
  if (value == 0) return std::nullopt;
  return static_cast<unsigned>(value);
}

std::optional<DispatchIdiom> matchPowerOfTwoDispatch(const Function& function) {
  if (!isI32(function.returnType()) || function.parameters().size() != 2 ||
      !isI32(function.parameters()[0].type) ||
      !isI32(function.parameters()[1].type) || function.blocks().size() < 5) {
    return std::nullopt;
  }

  std::unordered_map<std::string, const BasicBlock*> blocks;
  for (const auto& block : function.blocks()) {
    if (!blocks.emplace(block.name(), &block).second) return std::nullopt;
  }

  const std::string value = function.parameters()[0].name;
  const std::string shift = function.parameters()[1].name;
  const BasicBlock* current = &function.blocks().front();
  std::unordered_set<std::string> visited;
  std::string operation;
  unsigned expectedShift = 1;

  for (;;) {
    if (!visited.insert(current->name()).second) return std::nullopt;
    const auto& instructions = current->instructions();
    if (instructions.size() == 1 && instructions[0].opcode() == "ret" &&
        instructions[0].operands().size() == 1 &&
        instructions[0].operands()[0] == value) {
      break;
    }
    if (instructions.size() != 2 ||
        instructions[0].opcode() != "icmp.eq" ||
        instructions[0].operands().size() != 2 ||
        instructions[0].operands()[0] != shift ||
        instructions[1].opcode() != "br" ||
        instructions[1].operands().size() != 3 ||
        instructions[1].operands()[0] != instructions[0].result()) {
      return std::nullopt;
    }
    const auto compared = parsePositive(instructions[0].operands()[1]);
    if (!compared.has_value() || *compared != expectedShift ||
        expectedShift >= 31) {
      return std::nullopt;
    }
    const auto trueBlock = blocks.find(instructions[1].operands()[1]);
    const auto falseBlock = blocks.find(instructions[1].operands()[2]);
    if (trueBlock == blocks.end() || falseBlock == blocks.end() ||
        !visited.insert(trueBlock->first).second) {
      return std::nullopt;
    }
    const auto& selected = trueBlock->second->instructions();
    if (selected.size() != 2 || selected[0].operands().size() != 2 ||
        (selected[0].opcode() != "mul" && selected[0].opcode() != "sdiv") ||
        selected[0].operands()[0] != value ||
        selected[1].opcode() != "ret" || selected[1].operands().size() != 1 ||
        selected[1].operands()[0] != selected[0].result()) {
      return std::nullopt;
    }
    const auto factor = parsePositive(selected[0].operands()[1]);
    if (!factor.has_value() ||
        *factor != (std::uint32_t{1} << expectedShift)) {
      return std::nullopt;
    }
    if (operation.empty()) operation = selected[0].opcode();
    if (operation != selected[0].opcode()) return std::nullopt;
    current = falseBlock->second;
    ++expectedShift;
  }

  const unsigned maximumShift = expectedShift - 1;
  if (maximumShift < 2 || visited.size() != function.blocks().size()) {
    return std::nullopt;
  }
  return DispatchIdiom{operation, value, shift, maximumShift};
}

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) names.insert(block.name());
  for (unsigned suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  for (unsigned suffix = 0;; ++suffix) {
    const std::string candidate = "%" + stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

void addDispatchFastPath(Function& function, const DispatchIdiom& idiom) {
  const std::string originalEntry = function.blocks().front().name();
  const std::string guardName = freshBlockName(function, "pow2.dispatch.guard");
  const std::string fastName = freshBlockName(function, "pow2.dispatch.fast");
  const std::string lower = freshValueName(function, "pow2.dispatch.lower");
  const std::string upper = freshValueName(function, "pow2.dispatch.upper");
  const std::string inRange = freshValueName(function, "pow2.dispatch.range");
  const std::string nonnegative =
      freshValueName(function, "pow2.dispatch.nonnegative");
  const std::string eligible = freshValueName(function, "pow2.dispatch.eligible");
  const std::string result = freshValueName(function, "pow2.dispatch.result");

  BasicBlock guard{guardName};
  guard.addInstruction(Instruction{"icmp.sge", {idiom.shift, "1"}, lower});
  guard.addInstruction(Instruction{"icmp.sle",
                                   {idiom.shift,
                                    std::to_string(idiom.maximumShift)},
                                   upper});
  guard.addInstruction(Instruction{"and", {lower, upper}, inRange});
  std::string condition = inRange;
  if (idiom.operation == "sdiv") {
    guard.addInstruction(
        Instruction{"icmp.sge", {idiom.value, "0"}, nonnegative});
    guard.addInstruction(Instruction{"and", {inRange, nonnegative}, eligible});
    condition = eligible;
  }
  guard.addInstruction(Instruction{"br", {condition, fastName, originalEntry}});

  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{idiom.operation == "mul" ? "shl" : "lshr",
                                  {idiom.value, idiom.shift}, result});
  fast.addInstruction(Instruction{"ret", {result}});
  function.blocks().push_front(std::move(fast));
  function.blocks().push_front(std::move(guard));
}

ParameterRanges collectConstantParameterRanges(
    const compiler::ir::core::Module& module) {
  ParameterRanges ranges;
  std::unordered_map<std::string, const Function*> functions;
  for (const auto& function : module.functions()) {
    const std::string symbol = "@" + function.name();
    functions.emplace(symbol, &function);
    ranges.emplace(symbol,
                   std::vector<ParameterRange>(function.parameters().size()));
  }
  for (const auto& caller : module.functions()) {
    for (const auto& block : caller.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }
        const auto function = functions.find(instruction.operands().front());
        if (function == functions.end() ||
            instruction.operands().size() !=
                function->second->parameters().size() + 1) {
          continue;
        }
        auto& parameters = ranges.at(instruction.operands().front());
        for (std::size_t index = 0; index < parameters.size(); ++index) {
          auto& range = parameters[index];
          const auto value = parsePositive(instruction.operands()[index + 1]);
          if (!value.has_value()) {
            range.valid = false;
            continue;
          }
          if (!range.seen) {
            range.seen = true;
            range.lower = *value;
            range.upper = *value;
          } else {
            range.lower = std::min(range.lower, *value);
            range.upper = std::max(range.upper, *value);
          }
        }
      }
    }
  }
  return ranges;
}

std::optional<std::pair<unsigned, unsigned>> shiftRange(
    const Function& caller, const std::string& operand,
    const ParameterRanges& ranges) {
  if (const auto literal = parsePositive(operand); literal.has_value()) {
    return std::pair<unsigned, unsigned>{*literal, *literal};
  }
  const auto functionRanges = ranges.find("@" + caller.name());
  if (functionRanges == ranges.end()) return std::nullopt;
  for (std::size_t index = 0; index < caller.parameters().size(); ++index) {
    if (caller.parameters()[index].name != operand) continue;
    const auto& range = functionRanges->second[index];
    if (!range.seen || !range.valid) return std::nullopt;
    return std::pair<unsigned, unsigned>{range.lower, range.upper};
  }
  return std::nullopt;
}

void rewriteConstantDispatchCalls(
    compiler::ir::core::Module& module,
    const std::unordered_map<std::string, DispatchIdiom>& idioms,
    const ParameterRanges& ranges) {
  for (auto& function : module.functions()) {
    std::unordered_set<std::string> valueNames;
    for (const auto& parameter : function.parameters()) {
      valueNames.insert(parameter.name);
    }
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.hasResult()) valueNames.insert(instruction.result());
      }
    }
    for (auto& block : function.blocks()) {
      std::vector<Instruction> rewritten;
      rewritten.reserve(block.instructions().size());
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || !instruction.hasResult() ||
            instruction.operands().size() != 3) {
          rewritten.push_back(instruction);
          continue;
        }
        const auto idiom = idioms.find(instruction.operands()[0]);
        if (idiom == idioms.end()) {
          rewritten.push_back(instruction);
          continue;
        }
        const auto range = shiftRange(function, instruction.operands()[2], ranges);
        if (!range.has_value() || range->first < 1 ||
            range->second > idiom->second.maximumShift) {
          rewritten.push_back(instruction);
          continue;
        }
        if (idiom->second.operation == "mul") {
          rewritten.emplace_back(
              "shl",
              std::vector<std::string>{instruction.operands()[1],
                                       instruction.operands()[2]},
              instruction.result());
        } else {
          std::string divisor;
          if (range->first == range->second) {
            divisor = std::to_string(std::uint32_t{1} << range->first);
          } else {
            for (std::size_t suffix = 0;; ++suffix) {
              const std::string candidate =
                  "%pow2.dispatch.divisor." + std::to_string(suffix);
              if (valueNames.insert(candidate).second) {
                divisor = candidate;
                break;
              }
            }
            rewritten.emplace_back(
                "shl", std::vector<std::string>{"1", instruction.operands()[2]},
                divisor);
          }
          rewritten.emplace_back(
              "sdiv",
              std::vector<std::string>{instruction.operands()[1], divisor},
              instruction.result());
        }
      }
      block.instructions() = std::move(rewritten);
    }
  }
}

}  // namespace

void PowerOfTwoDispatchPass::run(compiler::ir::core::Module& module) {
  std::unordered_map<std::string, DispatchIdiom> idioms;
  for (const auto& function : module.functions()) {
    const auto idiom = matchPowerOfTwoDispatch(function);
    if (idiom.has_value()) idioms.emplace("@" + function.name(), *idiom);
  }
  if (std::getenv("COMPILER_DISABLE_CONSTANT_ARGUMENT_SPECIALIZATION") ==
      nullptr) {
    rewriteConstantDispatchCalls(module, idioms,
                                 collectConstantParameterRanges(module));
  }
  for (auto& function : module.functions()) {
    const auto idiom = idioms.find("@" + function.name());
    if (idiom != idioms.end()) addDispatchFastPath(function, idiom->second);
  }
}

}  // namespace compiler::midend::pass
