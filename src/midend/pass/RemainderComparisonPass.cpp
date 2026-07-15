#include "../../../include/compiler/midend/pass/RemainderComparisonPass.h"

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

bool isPositivePowerOfTwo(long long value) {
  if (value <= 0) {
    return false;
  }
  const auto unsignedValue = static_cast<std::uint64_t>(value);
  return (unsignedValue & (unsignedValue - 1U)) == 0;
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

std::string nextMaskName(std::unordered_set<std::string>& names, std::size_t& counter) {
  while (true) {
    const std::string candidate = "%rem.mask." + std::to_string(counter++);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

void simplifyRemainderComparisons(Function& function) {
  std::unordered_map<std::string, Instruction> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), instruction);
      }
    }
  }

  auto names = collectNames(function);
  std::size_t counter = 0;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      auto operands = instruction.operands();
      const bool isEquality = instruction.opcode() == "icmp.eq";
      const bool isInequality = instruction.opcode() == "icmp.ne";
      if ((isEquality || isInequality) && operands.size() == 2) {
        std::size_t remainderOperand = 0;
        long long zero = 1;
        if (detail::parseIntegerLiteral(operands[0], zero) && zero == 0) {
          remainderOperand = 1;
        } else if (!detail::parseIntegerLiteral(operands[1], zero) || zero != 0) {
          rewritten.emplace_back(instruction.opcode(), std::move(operands),
                                 instruction.result());
          continue;
        }

        const auto found = definitions.find(operands[remainderOperand]);
        if (found != definitions.end()) {
          const auto& remainder = found->second;
          long long divisor = 0;
          if ((remainder.opcode() == "mod" || remainder.opcode() == "srem") &&
              remainder.operands().size() == 2 &&
              detail::parseIntegerLiteral(remainder.operands()[1], divisor) &&
              isPositivePowerOfTwo(divisor)) {
            const std::string maskResult = nextMaskName(names, counter);
            std::string valueToMask = remainder.operands()[0];
            if (divisor == 2) {
              const auto dividend = definitions.find(remainder.operands()[0]);
              if (dividend != definitions.end() &&
                  dividend->second.opcode() == "mul" &&
                  dividend->second.operands().size() == 2) {
                // Multiplication modulo two is multiplication in GF(2), i.e.
                // the AND of the operands' low bits.  Retain the final mask
                // so the replacement has exactly the original 0/1 value for
                // signed as well as unsigned two's-complement operands.
                valueToMask = nextMaskName(names, counter);
                rewritten.emplace_back(
                    "and",
                    std::vector<std::string>{dividend->second.operands()[0],
                                             dividend->second.operands()[1]},
                    valueToMask);
              }
            }
            rewritten.emplace_back(
                "and",
                std::vector<std::string>{valueToMask,
                                         std::to_string(divisor - 1)},
                maskResult);
            operands[remainderOperand] = maskResult;
          }
        }
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

}  // namespace

void RemainderComparisonPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    simplifyRemainderComparisons(function);
  }
}

}  // namespace compiler::midend::pass
