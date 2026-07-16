#include "../../../include/compiler/midend/pass/DivRemPairStrengthReductionPass.h"

#include <cstdint>
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

struct PairKey {
  std::string dividend;
  std::uint64_t divisorMagnitude = 0;

  bool operator==(const PairKey& other) const {
    return dividend == other.dividend &&
           divisorMagnitude == other.divisorMagnitude;
  }
};

struct PairKeyHash {
  std::size_t operator()(const PairKey& key) const {
    const auto dividendHash = std::hash<std::string>{}(key.dividend);
    const auto magnitudeHash = std::hash<std::uint64_t>{}(key.divisorMagnitude);
    return dividendHash ^ (magnitudeHash + 0x9e3779b9U + (dividendHash << 6U) +
                           (dividendHash >> 2U));
  }
};

bool powerOfTwoMagnitude(long long value, std::uint64_t& magnitude,
                         unsigned& shift) {
  if (value == 0) {
    return false;
  }
  magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                        : static_cast<std::uint64_t>(value);
  if (magnitude < 2U || (magnitude & (magnitude - 1U)) != 0U) {
    return false;
  }
  shift = 0;
  for (auto current = magnitude; current > 1U; current >>= 1U) {
    ++shift;
  }
  return shift < 64U;
}

std::unordered_set<std::string> collectNames(const Function& function) {
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

std::string freshName(std::unordered_set<std::string>& names,
                      const std::string& stem, std::size_t& counter) {
  while (true) {
    const std::string candidate = stem + "." + std::to_string(counter++);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

bool isRemainder(const Instruction& instruction) {
  return (instruction.opcode() == "mod" || instruction.opcode() == "srem") &&
         instruction.hasResult() && instruction.operands().size() == 2;
}

bool isDivision(const Instruction& instruction) {
  return (instruction.opcode() == "div" || instruction.opcode() == "sdiv") &&
         instruction.hasResult() && instruction.operands().size() == 2;
}

void reducePairs(Function& function) {
  auto names = collectNames(function);
  std::size_t counter = 0;

  for (auto& block : function.blocks()) {
    std::unordered_map<PairKey, std::string, PairKeyHash> precedingRemainders;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      long long divisor = 0;
      std::uint64_t magnitude = 0;
      unsigned shift = 0;
      const bool hasPowerOfTwoDivisor =
          instruction.operands().size() == 2 &&
          detail::parseIntegerLiteral(instruction.operands()[1], divisor) &&
          powerOfTwoMagnitude(divisor, magnitude, shift);

      if (isDivision(instruction) && hasPowerOfTwoDivisor) {
        const PairKey key{instruction.operands()[0], magnitude};
        const auto remainder = precedingRemainders.find(key);
        if (remainder != precedingRemainders.end()) {
          const auto adjusted = freshName(names, "%divrem.adjusted", counter);
          rewritten.emplace_back(
              "sub",
              std::vector<std::string>{instruction.operands()[0],
                                       remainder->second},
              adjusted);
          if (divisor > 0) {
            rewritten.emplace_back(
                "ashr",
                std::vector<std::string>{adjusted, std::to_string(shift)},
                instruction.result());
          } else {
            const auto quotient =
                freshName(names, "%divrem.positive.quotient", counter);
            rewritten.emplace_back(
                "ashr",
                std::vector<std::string>{adjusted, std::to_string(shift)},
                quotient);
            rewritten.emplace_back("neg", std::vector<std::string>{quotient},
                                   instruction.result());
          }
          continue;
        }
      }

      rewritten.push_back(instruction);
      if (isRemainder(instruction) && hasPowerOfTwoDivisor) {
        precedingRemainders.insert_or_assign(
            PairKey{instruction.operands()[0], magnitude},
            instruction.result());
      }
    }
    block.instructions() = std::move(rewritten);
  }
}

}  // namespace

void DivRemPairStrengthReductionPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    reducePairs(function);
  }
}

}  // namespace compiler::midend::pass
