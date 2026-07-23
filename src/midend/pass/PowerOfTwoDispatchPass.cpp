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

struct RepeatedDivisionDigitIdiom {
  std::string value;
  std::string count;
  unsigned divisor = 0;
  unsigned divisorShift = 0;
  std::size_t valueParameter = 0;
  std::size_t countParameter = 0;
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

  const bool includeZeroShift =
      idiom.operation == "mul" &&
      std::getenv("COMPILER_DISABLE_ZERO_POWER_DISPATCH") == nullptr;
  BasicBlock guard{guardName};
  guard.addInstruction(Instruction{
      "icmp.sge", {idiom.shift, includeZeroShift ? "0" : "1"}, lower});
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

void replaceMultiplicationDispatch(Function& function,
                                   const DispatchIdiom& idiom) {
  const std::string entryName = function.blocks().front().name();
  const std::string lower = freshValueName(function, "pow2.closed.lower");
  const std::string upper = freshValueName(function, "pow2.closed.upper");
  const std::string inRange = freshValueName(function, "pow2.closed.range");
  const std::string safeShift =
      freshValueName(function, "pow2.closed.safe.shift");
  const std::string result = freshValueName(function, "pow2.closed.result");
  BasicBlock entry{entryName};
  entry.addInstruction(Instruction{"icmp.sge", {idiom.shift, "0"}, lower});
  entry.addInstruction(Instruction{
      "icmp.sle", {idiom.shift, std::to_string(idiom.maximumShift)}, upper});
  entry.addInstruction(Instruction{"and", {lower, upper}, inRange});
  entry.addInstruction(
      Instruction{"select", {inRange, idiom.shift, "0"}, safeShift});
  entry.addInstruction(
      Instruction{"shl", {idiom.value, safeShift}, result});
  entry.addInstruction(Instruction{"ret", {result}});
  function.blocks().clear();
  function.addBlock(std::move(entry));
}

void appendSignedPowerOfTwoDivision(
    std::vector<Instruction>& instructions, const std::string& value,
    const std::string& shift, const std::string& result,
    std::unordered_set<std::string>& valueNames) {
  const auto fresh = [&](const std::string& stem) {
    for (std::size_t suffix = 0;; ++suffix) {
      const std::string candidate =
          "%pow2.sdiv." + stem + "." + std::to_string(suffix);
      if (valueNames.insert(candidate).second) return candidate;
    }
  };
  const std::string power = fresh("power");
  const std::string mask = fresh("mask");
  const std::string sign = fresh("sign");
  const std::string bias = fresh("bias");
  const std::string adjusted = fresh("adjusted");
  instructions.emplace_back("shl", std::vector<std::string>{"1", shift},
                            power);
  instructions.emplace_back("sub", std::vector<std::string>{power, "1"},
                            mask);
  instructions.emplace_back("ashr", std::vector<std::string>{value, "31"},
                            sign);
  instructions.emplace_back("and", std::vector<std::string>{sign, mask}, bias);
  instructions.emplace_back("add", std::vector<std::string>{value, bias},
                            adjusted);
  instructions.emplace_back("ashr",
                            std::vector<std::string>{adjusted, shift}, result);
}

void replaceDivisionDispatch(Function& function, const DispatchIdiom& idiom) {
  const std::string entryName = function.blocks().front().name();
  const std::string lower = freshValueName(function, "pow2.sdiv.closed.lower");
  const std::string upper = freshValueName(function, "pow2.sdiv.closed.upper");
  const std::string inRange =
      freshValueName(function, "pow2.sdiv.closed.range");
  const std::string safeShift =
      freshValueName(function, "pow2.sdiv.closed.safe.shift");
  const std::string result =
      freshValueName(function, "pow2.sdiv.closed.result");
  std::unordered_set<std::string> valueNames;
  for (const auto& parameter : function.parameters()) {
    valueNames.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) valueNames.insert(instruction.result());
    }
  }
  valueNames.insert(lower);
  valueNames.insert(upper);
  valueNames.insert(inRange);
  valueNames.insert(safeShift);
  valueNames.insert(result);

  BasicBlock entry{entryName};
  entry.addInstruction(Instruction{"icmp.sge", {idiom.shift, "1"}, lower});
  entry.addInstruction(Instruction{
      "icmp.sle", {idiom.shift, std::to_string(idiom.maximumShift)}, upper});
  entry.addInstruction(Instruction{"and", {lower, upper}, inRange});
  entry.addInstruction(
      Instruction{"select", {inRange, idiom.shift, "0"}, safeShift});
  appendSignedPowerOfTwoDivision(entry.instructions(), idiom.value, safeShift,
                                 result, valueNames);
  entry.addInstruction(Instruction{"ret", {result}});
  function.blocks().clear();
  function.addBlock(std::move(entry));
}

std::optional<std::string> phiIncomingValue(const Instruction& phi,
                                            const std::string& predecessor) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5) {
    return std::nullopt;
  }
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) {
      return phi.operands()[index];
    }
  }
  return std::nullopt;
}

std::optional<unsigned> positivePowerOfTwoShift(const std::string& text) {
  const auto value = parsePositive(text);
  if (!value.has_value() || *value < 2 ||
      (*value & (*value - 1U)) != 0) {
    return std::nullopt;
  }
  unsigned shift = 0;
  for (unsigned remaining = *value; remaining > 1; remaining >>= 1U) {
    ++shift;
  }
  return shift < 31 ? std::optional<unsigned>{shift} : std::nullopt;
}

std::optional<RepeatedDivisionDigitIdiom>
matchRepeatedPowerOfTwoDivisionDigit(const Function& function) {
  if (!isI32(function.returnType()) || function.parameters().size() != 2 ||
      !isI32(function.parameters()[0].type) ||
      !isI32(function.parameters()[1].type) ||
      function.blocks().size() != 4) {
    return std::nullopt;
  }

  std::unordered_map<std::string, const BasicBlock*> blocks;
  for (const auto& block : function.blocks()) {
    if (!blocks.emplace(block.name(), &block).second) return std::nullopt;
  }

  const auto& entry = function.blocks().front();
  if (entry.instructions().size() != 1 ||
      entry.instructions().front().opcode() != "br" ||
      entry.instructions().front().operands().size() != 1) {
    return std::nullopt;
  }
  const auto headerFound =
      blocks.find(entry.instructions().front().operands().front());
  if (headerFound == blocks.end()) return std::nullopt;
  const auto& header = *headerFound->second;
  if (header.instructions().size() != 4 ||
      header.instructions()[0].opcode() != "phi" ||
      header.instructions()[1].opcode() != "phi" ||
      header.instructions()[2].opcode() != "icmp.slt" ||
      header.instructions()[2].operands().size() != 2 ||
      header.instructions()[3].opcode() != "br" ||
      header.instructions()[3].operands().size() != 3 ||
      header.instructions()[3].operands()[0] !=
          header.instructions()[2].result()) {
    return std::nullopt;
  }

  const auto bodyFound = blocks.find(header.instructions()[3].operands()[1]);
  const auto exitFound = blocks.find(header.instructions()[3].operands()[2]);
  if (bodyFound == blocks.end() || exitFound == blocks.end()) {
    return std::nullopt;
  }
  const auto& body = *bodyFound->second;
  const auto& exit = *exitFound->second;
  if (body.instructions().size() != 3 ||
      body.instructions().back().opcode() != "br" ||
      body.instructions().back().operands() !=
          std::vector<std::string>{header.name()} ||
      exit.instructions().size() != 2 ||
      exit.instructions()[0].opcode() != "srem" ||
      exit.instructions()[0].operands().size() != 2 ||
      exit.instructions()[1].opcode() != "ret" ||
      exit.instructions()[1].operands() !=
          std::vector<std::string>{exit.instructions()[0].result()}) {
    return std::nullopt;
  }

  const Instruction* division = nullptr;
  const Instruction* increment = nullptr;
  for (std::size_t index = 0; index < 2; ++index) {
    const auto& instruction = body.instructions()[index];
    if (instruction.opcode() == "sdiv" &&
        instruction.operands().size() == 2) {
      division = &instruction;
    } else if (instruction.opcode() == "add" &&
               instruction.operands().size() == 2) {
      increment = &instruction;
    }
  }
  if (division == nullptr || increment == nullptr) return std::nullopt;

  const Instruction* counterPhi = nullptr;
  const Instruction* valuePhi = nullptr;
  for (std::size_t index = 0; index < 2; ++index) {
    const auto& phi = header.instructions()[index];
    const auto entryValue = phiIncomingValue(phi, entry.name());
    const auto bodyValue = phiIncomingValue(phi, body.name());
    if (!entryValue.has_value() || !bodyValue.has_value()) {
      return std::nullopt;
    }
    const bool incrementsCounter =
        *entryValue == "0" && *bodyValue == increment->result() &&
        ((increment->operands()[0] == phi.result() &&
          increment->operands()[1] == "1") ||
         (increment->operands()[1] == phi.result() &&
          increment->operands()[0] == "1"));
    if (incrementsCounter) {
      counterPhi = &phi;
      continue;
    }
    const bool carriesQuotient =
        *bodyValue == division->result() &&
        division->operands()[0] == phi.result() &&
        (*entryValue == function.parameters()[0].name ||
         *entryValue == function.parameters()[1].name);
    if (carriesQuotient) valuePhi = &phi;
  }
  if (counterPhi == nullptr || valuePhi == nullptr ||
      header.instructions()[2].operands()[0] != counterPhi->result() ||
      exit.instructions()[0].operands()[0] != valuePhi->result() ||
      exit.instructions()[0].operands()[1] != division->operands()[1]) {
    return std::nullopt;
  }

  const auto valueIncoming = phiIncomingValue(*valuePhi, entry.name());
  if (!valueIncoming.has_value()) return std::nullopt;
  std::string count;
  for (const auto& parameter : function.parameters()) {
    if (parameter.name != *valueIncoming) count = parameter.name;
  }
  if (count.empty() || header.instructions()[2].operands()[1] != count) {
    return std::nullopt;
  }
  const auto divisorShift =
      positivePowerOfTwoShift(division->operands()[1]);
  const auto divisor = parsePositive(division->operands()[1]);
  if (!divisorShift.has_value() || !divisor.has_value()) {
    return std::nullopt;
  }
  std::size_t valueParameter = function.parameters().size();
  std::size_t countParameter = function.parameters().size();
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    if (function.parameters()[index].name == *valueIncoming) {
      valueParameter = index;
    }
    if (function.parameters()[index].name == count) {
      countParameter = index;
    }
  }
  if (valueParameter == function.parameters().size() ||
      countParameter == function.parameters().size()) {
    return std::nullopt;
  }
  return RepeatedDivisionDigitIdiom{*valueIncoming, count, *divisor,
                                    *divisorShift, valueParameter,
                                    countParameter};
}

void replaceRepeatedPowerOfTwoDivisionDigit(
    Function& function, const RepeatedDivisionDigitIdiom& idiom) {
  const std::string entryName = function.blocks().front().name();
  const std::string noLoopName =
      freshBlockName(function, "pow2.repeat.no_loop");
  const std::string rangeName =
      freshBlockName(function, "pow2.repeat.range");
  const std::string zeroName =
      freshBlockName(function, "pow2.repeat.zero");
  const std::string fastName =
      freshBlockName(function, "pow2.repeat.fast");
  const std::string nonpositive =
      freshValueName(function, "pow2.repeat.nonpositive");
  const std::string reachesZero =
      freshValueName(function, "pow2.repeat.reaches.zero");
  const std::string shift =
      freshValueName(function, "pow2.repeat.shift");
  const std::string quotient =
      freshValueName(function, "pow2.repeat.quotient");
  const std::string noLoopResult =
      freshValueName(function, "pow2.repeat.no_loop.result");
  const std::string fastResult =
      freshValueName(function, "pow2.repeat.fast.result");
  const unsigned zeroThreshold =
      (32U + idiom.divisorShift - 1U) / idiom.divisorShift;

  std::unordered_set<std::string> valueNames;
  for (const auto& parameter : function.parameters()) {
    valueNames.insert(parameter.name);
  }
  valueNames.insert(nonpositive);
  valueNames.insert(reachesZero);
  valueNames.insert(shift);
  valueNames.insert(quotient);
  valueNames.insert(noLoopResult);
  valueNames.insert(fastResult);

  BasicBlock entry{entryName};
  entry.addInstruction(
      Instruction{"icmp.sle", {idiom.count, "0"}, nonpositive});
  entry.addInstruction(
      Instruction{"br", {nonpositive, noLoopName, rangeName}});

  BasicBlock noLoop{noLoopName};
  noLoop.addInstruction(Instruction{
      "srem", {idiom.value, std::to_string(idiom.divisor)}, noLoopResult});
  noLoop.addInstruction(Instruction{"ret", {noLoopResult}});

  BasicBlock range{rangeName};
  range.addInstruction(
      Instruction{"icmp.sge",
                  {idiom.count, std::to_string(zeroThreshold)}, reachesZero});
  range.addInstruction(Instruction{"br", {reachesZero, zeroName, fastName}});

  BasicBlock zero{zeroName};
  zero.addInstruction(Instruction{"ret", {"0"}});

  BasicBlock fast{fastName};
  if (idiom.divisorShift == 1) {
    appendSignedPowerOfTwoDivision(fast.instructions(), idiom.value,
                                   idiom.count, quotient, valueNames);
  } else {
    fast.addInstruction(
        Instruction{"mul",
                    {idiom.count, std::to_string(idiom.divisorShift)}, shift});
    appendSignedPowerOfTwoDivision(fast.instructions(), idiom.value, shift,
                                   quotient, valueNames);
  }
  fast.addInstruction(Instruction{
      "srem", {quotient, std::to_string(idiom.divisor)}, fastResult});
  fast.addInstruction(Instruction{"ret", {fastResult}});

  function.blocks().clear();
  function.addBlock(std::move(entry));
  function.addBlock(std::move(noLoop));
  function.addBlock(std::move(range));
  function.addBlock(std::move(zero));
  function.addBlock(std::move(fast));
}

void rewriteRepeatedPowerOfTwoDivisionDigitCalls(
    compiler::ir::core::Module& module,
    const std::unordered_map<std::string, RepeatedDivisionDigitIdiom>&
        idioms) {
  constexpr std::size_t kMaximumExpandedCallSites = 16;
  std::unordered_map<std::string, std::size_t> callSiteCounts;
  for (const auto& function : module.functions()) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() == "call" &&
            !instruction.operands().empty() &&
            idioms.find(instruction.operands().front()) != idioms.end()) {
          ++callSiteCounts[instruction.operands().front()];
        }
      }
    }
  }

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
    const auto fresh = [&](const std::string& stem) {
      for (std::size_t suffix = 0;; ++suffix) {
        const std::string candidate =
            "%pow2.repeat.call." + stem + "." + std::to_string(suffix);
        if (valueNames.insert(candidate).second) return candidate;
      }
    };

    for (auto& block : function.blocks()) {
      std::vector<Instruction> rewritten;
      rewritten.reserve(block.instructions().size());
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "call" || !instruction.hasResult() ||
            instruction.operands().empty()) {
          rewritten.push_back(instruction);
          continue;
        }
        const auto found = idioms.find(instruction.operands().front());
        if (found == idioms.end() ||
            instruction.operands().size() != 3 ||
            callSiteCounts[instruction.operands().front()] >
                kMaximumExpandedCallSites) {
          rewritten.push_back(instruction);
          continue;
        }
        const auto& idiom = found->second;
        const std::string& value =
            instruction.operands()[idiom.valueParameter + 1];
        const std::string& count =
            instruction.operands()[idiom.countParameter + 1];
        const unsigned zeroThreshold =
            (32U + idiom.divisorShift - 1U) / idiom.divisorShift;
        const std::string positive = fresh("positive");
        const std::string belowZeroThreshold = fresh("below.zero");
        const std::string active = fresh("active");
        const std::string safeCount = fresh("safe.count");
        const std::string shift = fresh("shift");
        const std::string quotient = fresh("quotient");
        const std::string remainder = fresh("remainder");
        const std::string reachesZero = fresh("reaches.zero");

        rewritten.emplace_back("icmp.sgt",
                               std::vector<std::string>{count, "0"},
                               positive);
        rewritten.emplace_back(
            "icmp.slt",
            std::vector<std::string>{
                count, std::to_string(zeroThreshold)},
            belowZeroThreshold);
        rewritten.emplace_back(
            "and", std::vector<std::string>{positive, belowZeroThreshold},
            active);
        rewritten.emplace_back(
            "select", std::vector<std::string>{active, count, "0"},
            safeCount);
        std::string safeShift = safeCount;
        if (idiom.divisorShift != 1) {
          rewritten.emplace_back(
              "mul",
              std::vector<std::string>{
                  safeCount, std::to_string(idiom.divisorShift)},
              shift);
          safeShift = shift;
        }
        appendSignedPowerOfTwoDivision(rewritten, value, safeShift, quotient,
                                       valueNames);
        rewritten.emplace_back(
            "srem",
            std::vector<std::string>{quotient,
                                     std::to_string(idiom.divisor)},
            remainder);
        rewritten.emplace_back(
            "icmp.sge",
            std::vector<std::string>{
                count, std::to_string(zeroThreshold)},
            reachesZero);
        rewritten.emplace_back(
            "select",
            std::vector<std::string>{reachesZero, "0", remainder},
            instruction.result());
      }
      block.instructions() = std::move(rewritten);
    }
  }
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
        } else if (range->first == range->second) {
          const std::string divisor =
              std::to_string(std::uint32_t{1} << range->first);
          rewritten.emplace_back(
              "sdiv",
              std::vector<std::string>{instruction.operands()[1], divisor},
              instruction.result());
        } else if (
            std::getenv("COMPILER_DISABLE_CLOSED_SIGNED_DIVISION_DISPATCH") ==
                nullptr &&
            std::getenv("COMPILER_DISABLE_CLOSED_HELPER_STATE_PIPELINE") ==
                nullptr) {
          appendSignedPowerOfTwoDivision(
              rewritten, instruction.operands()[1], instruction.operands()[2],
              instruction.result(), valueNames);
        } else {
          std::string divisor;
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
  if (std::getenv("COMPILER_DISABLE_REPEATED_POWER_OF_TWO_DIVISION") ==
      nullptr) {
    std::unordered_map<std::string, RepeatedDivisionDigitIdiom>
        repeatedDivisionIdioms;
    for (const auto& function : module.functions()) {
      const auto idiom = matchRepeatedPowerOfTwoDivisionDigit(function);
      if (idiom.has_value()) {
        repeatedDivisionIdioms.emplace("@" + function.name(), *idiom);
      }
    }
    rewriteRepeatedPowerOfTwoDivisionDigitCalls(module,
                                                repeatedDivisionIdioms);
    for (auto& function : module.functions()) {
      const auto idiom =
          repeatedDivisionIdioms.find("@" + function.name());
      if (idiom != repeatedDivisionIdioms.end()) {
        replaceRepeatedPowerOfTwoDivisionDigit(function, idiom->second);
      }
    }
  }
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
    if (idiom == idioms.end()) continue;
    if (std::getenv("COMPILER_DISABLE_CLOSED_HELPER_STATE_PIPELINE") == nullptr) {
      if (idiom->second.operation == "mul" &&
          std::getenv("COMPILER_DISABLE_CLOSED_MULTIPLY_DISPATCH") == nullptr) {
        replaceMultiplicationDispatch(function, idiom->second);
      } else if (idiom->second.operation == "sdiv" &&
                 std::getenv("COMPILER_DISABLE_CLOSED_SIGNED_DIVISION_DISPATCH") ==
                     nullptr) {
        replaceDivisionDispatch(function, idiom->second);
      } else {
        addDispatchFastPath(function, idiom->second);
      }
    } else {
      addDispatchFastPath(function, idiom->second);
    }
  }
}

}  // namespace compiler::midend::pass
