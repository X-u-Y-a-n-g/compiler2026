#include "../../../include/compiler/midend/pass/AArch64PureMapRecurrenceVectorizationPass.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr int kVectorWidth = 4;
constexpr int kPrefixScanWidth = 32;
constexpr int kMaximumVectorSlots = 14;
constexpr std::size_t kMaximumTransformsPerFunction = 8;

struct Definition {
  const Instruction *instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct Dependency {
  bool state = false;
  bool induction = false;
};

struct SignedMagic {
  std::int32_t multiplier = 0;
  unsigned shift = 0;
};

struct VectorOperation {
  enum class Kind {
    Binary,
    Divide,
    Remainder,
  };

  Kind kind = Kind::Binary;
  int opcode = 0;
  int destination = 0;
  bool lhsImmediate = false;
  std::int32_t lhs = 0;
  bool rhsImmediate = false;
  std::int32_t rhs = 0;
  std::int32_t divisor = 0;
  SignedMagic magic;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string preheader;
  std::string exit;
  std::string induction;
  std::string state;
  std::string inductionBackedge;
  std::string stateBackedge;
  std::string initialState;
  std::string step;
  std::string mapRoot;
  int mapRootSlot = 0;
  std::vector<VectorOperation> vectorOperations;
  std::vector<const Instruction *> recurrenceInstructions;
};

struct IntegerInterval {
  std::int64_t minimum = std::numeric_limits<std::int32_t>::min();
  std::int64_t maximum = std::numeric_limits<std::int32_t>::max();
};

struct BoundedRemainderScan {
  std::int32_t modulus = 0;
  std::int64_t mapCoefficient = 0;
  std::int64_t constant = 0;
  IntegerInterval increment;
};

// A pure map of the following algebraic family can be evaluated in quotient
// blocks instead of once per induction value:
//
//   value = max(x, C0 - x)
//   value = max(value, C1 - value) ...  // proven redundant on x >= 0
//   q     = wrap(A * value) / D
//   map   = wrap(value + wrap(q * E)) % P
//
// Constants are deliberately kept symbolic here.  The matcher accepts an
// arbitrary number of mirror/max stages and arbitrary safe i32 constants; the
// optimization is selected from SSA semantics rather than symbol identity.
struct QuotientBlockSummary {
  std::int32_t mirror = 0;
  std::int32_t multiplier = 0;
  std::int32_t divisor = 0;
  std::int32_t quotientMultiplier = 0;
  std::int32_t mapModulus = 0;
  std::int32_t stateModulus = 0;
  std::int64_t mapCoefficient = 0;
  std::int64_t incrementConstant = 0;
};

DefinitionMap definitionsOf(const Function &function) {
  DefinitionMap definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
      }
    }
  }
  return definitions;
}

std::unordered_map<std::string, const BasicBlock *>
blocksOf(const Function &function) {
  std::unordered_map<std::string, const BasicBlock *> blocks;
  for (const auto &block : function.blocks()) {
    blocks.emplace(block.name(), &block);
  }
  return blocks;
}

std::unordered_map<std::string, std::vector<std::string>>
predecessorsOf(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto &block : function.blocks()) {
    predecessors.emplace(block.name(), std::vector<std::string>{});
  }
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto &operands = terminator->operands();
    const std::size_t begin = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = begin; index < operands.size(); ++index) {
      const auto found = predecessors.find(operands[index]);
      if (found != predecessors.end()) {
        found->second.push_back(block.name());
      }
    }
  }
  return predecessors;
}

std::optional<std::string> phiIncoming(const Instruction &phi,
                                       const std::string &predecessor) {
  if (phi.opcode() != "phi") {
    return std::nullopt;
  }
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) {
      return phi.operands()[index];
    }
  }
  return std::nullopt;
}

bool parseI32(const std::string &text, std::int32_t &value) {
  long long parsed = 0;
  if (!detail::parseIntegerLiteral(text, parsed) ||
      parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  value = static_cast<std::int32_t>(parsed);
  return true;
}

bool isValue(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

SignedMagic signedMagicForPositiveDivisor(std::uint32_t divisor) {
  const std::uint64_t two31 = std::uint64_t{1} << 31;
  const std::uint64_t anc = two31 - 1 - (two31 - 1) % divisor;
  unsigned p = 31;
  std::uint64_t q1 = two31 / anc;
  std::uint64_t r1 = two31 - q1 * anc;
  std::uint64_t q2 = two31 / divisor;
  std::uint64_t r2 = two31 - q2 * divisor;
  while (true) {
    ++p;
    q1 *= 2;
    r1 *= 2;
    if (r1 >= anc) {
      ++q1;
      r1 -= anc;
    }
    q2 *= 2;
    r2 *= 2;
    if (r2 >= divisor) {
      ++q2;
      r2 -= divisor;
    }
    const std::uint64_t delta = divisor - r2;
    if (!(q1 < delta || (q1 == delta && r1 == 0))) {
      break;
    }
  }
  return SignedMagic{static_cast<std::int32_t>(q2 + 1), p - 32};
}

std::optional<int> binaryOpcode(const std::string &opcode) {
  if (opcode == "add")
    return 0;
  if (opcode == "sub")
    return 1;
  if (opcode == "mul")
    return 2;
  return std::nullopt;
}

IntegerInterval fullI32Interval() {
  return {std::numeric_limits<std::int32_t>::min(),
          std::numeric_limits<std::int32_t>::max()};
}

bool fitsI32(__int128 value) {
  return value >= std::numeric_limits<std::int32_t>::min() &&
         value <= std::numeric_limits<std::int32_t>::max();
}

std::optional<IntegerInterval> exactBinaryInterval(int opcode,
                                                   const IntegerInterval &lhs,
                                                   const IntegerInterval &rhs) {
  __int128 minimum = 0;
  __int128 maximum = 0;
  if (opcode == 0) {
    minimum = static_cast<__int128>(lhs.minimum) + rhs.minimum;
    maximum = static_cast<__int128>(lhs.maximum) + rhs.maximum;
  } else if (opcode == 1) {
    minimum = static_cast<__int128>(lhs.minimum) - rhs.maximum;
    maximum = static_cast<__int128>(lhs.maximum) - rhs.minimum;
  } else if (opcode == 2) {
    const std::array<__int128, 4> products{
        static_cast<__int128>(lhs.minimum) * rhs.minimum,
        static_cast<__int128>(lhs.minimum) * rhs.maximum,
        static_cast<__int128>(lhs.maximum) * rhs.minimum,
        static_cast<__int128>(lhs.maximum) * rhs.maximum};
    minimum = *std::min_element(products.begin(), products.end());
    maximum = *std::max_element(products.begin(), products.end());
  } else if (opcode == 3) {
    minimum = std::max(lhs.minimum, rhs.minimum);
    maximum = std::max(lhs.maximum, rhs.maximum);
  } else if (opcode == 4) {
    minimum = std::min(lhs.minimum, rhs.minimum);
    maximum = std::min(lhs.maximum, rhs.maximum);
  } else {
    return std::nullopt;
  }
  if (!fitsI32(minimum) || !fitsI32(maximum)) {
    return fullI32Interval();
  }
  return IntegerInterval{static_cast<std::int64_t>(minimum),
                         static_cast<std::int64_t>(maximum)};
}

std::optional<IntegerInterval> intervalForMapRoot(const Candidate &candidate) {
  std::array<std::optional<IntegerInterval>, kMaximumVectorSlots> intervals;
  intervals[0] = fullI32Interval();
  const auto operandInterval =
      [&](bool immediate,
          std::int32_t encoded) -> std::optional<IntegerInterval> {
    if (immediate) {
      return IntegerInterval{encoded, encoded};
    }
    if (encoded < 0 || encoded >= kMaximumVectorSlots) {
      return std::nullopt;
    }
    return intervals[static_cast<std::size_t>(encoded)];
  };

  for (const auto &operation : candidate.vectorOperations) {
    if (operation.destination < 0 ||
        operation.destination >= kMaximumVectorSlots) {
      return std::nullopt;
    }
    const auto lhs = operandInterval(operation.lhsImmediate, operation.lhs);
    if (!lhs.has_value()) {
      return std::nullopt;
    }
    IntegerInterval result;
    if (operation.kind == VectorOperation::Kind::Binary) {
      const auto rhs = operandInterval(operation.rhsImmediate, operation.rhs);
      if (!rhs.has_value()) {
        return std::nullopt;
      }
      const auto computed = exactBinaryInterval(operation.opcode, *lhs, *rhs);
      if (!computed.has_value()) {
        return std::nullopt;
      }
      result = *computed;
    } else if (operation.kind == VectorOperation::Kind::Divide) {
      result = {lhs->minimum / operation.divisor,
                lhs->maximum / operation.divisor};
    } else {
      const std::int64_t span =
          static_cast<std::int64_t>(operation.divisor) - 1;
      if (lhs->minimum >= 0) {
        result = {0, std::min(lhs->maximum, span)};
      } else if (lhs->maximum <= 0) {
        result = {std::max(lhs->minimum, -span), 0};
      } else {
        result = {-span, span};
      }
    }
    intervals[static_cast<std::size_t>(operation.destination)] = result;
  }
  if (candidate.mapRootSlot < 0 ||
      candidate.mapRootSlot >= kMaximumVectorSlots) {
    return std::nullopt;
  }
  return intervals[static_cast<std::size_t>(candidate.mapRootSlot)];
}

struct AffineIncrement {
  std::int64_t state = 0;
  std::int64_t map = 0;
  std::int64_t constant = 0;
};

bool addAffine(const AffineIncrement &lhs, const AffineIncrement &rhs, int sign,
               AffineIncrement &result) {
  const __int128 state = static_cast<__int128>(lhs.state) + sign * rhs.state;
  const __int128 map = static_cast<__int128>(lhs.map) + sign * rhs.map;
  const __int128 constant =
      static_cast<__int128>(lhs.constant) + sign * rhs.constant;
  if (state < std::numeric_limits<std::int64_t>::min() ||
      state > std::numeric_limits<std::int64_t>::max() ||
      map < std::numeric_limits<std::int64_t>::min() ||
      map > std::numeric_limits<std::int64_t>::max() ||
      constant < std::numeric_limits<std::int64_t>::min() ||
      constant > std::numeric_limits<std::int64_t>::max()) {
    return false;
  }
  result = {static_cast<std::int64_t>(state), static_cast<std::int64_t>(map),
            static_cast<std::int64_t>(constant)};
  return true;
}

std::optional<BoundedRemainderScan>
boundedRemainderScanFor(const Candidate &candidate) {
  if (candidate.recurrenceInstructions.size() < 2) {
    return std::nullopt;
  }
  const auto *remainder = candidate.recurrenceInstructions.back();
  std::int32_t modulus = 0;
  if (remainder->opcode() != "srem" || remainder->operands().size() != 2 ||
      !parseI32(remainder->operands()[1], modulus) || modulus <= 1) {
    return std::nullopt;
  }

  std::unordered_map<std::string, AffineIncrement> expressions;
  expressions.emplace(candidate.state, AffineIncrement{1, 0, 0});
  expressions.emplace(candidate.mapRoot, AffineIncrement{0, 1, 0});
  const auto affineOperand =
      [&](const std::string &operand) -> std::optional<AffineIncrement> {
    const auto found = expressions.find(operand);
    if (found != expressions.end()) {
      return found->second;
    }
    std::int32_t immediate = 0;
    if (parseI32(operand, immediate)) {
      return AffineIncrement{0, 0, immediate};
    }
    return std::nullopt;
  };

  for (std::size_t index = 0;
       index + 1 < candidate.recurrenceInstructions.size(); ++index) {
    const auto *instruction = candidate.recurrenceInstructions[index];
    if ((instruction->opcode() != "add" && instruction->opcode() != "sub") ||
        instruction->operands().size() != 2) {
      return std::nullopt;
    }
    const auto lhs = affineOperand(instruction->operands()[0]);
    const auto rhs = affineOperand(instruction->operands()[1]);
    AffineIncrement result;
    if (!lhs.has_value() || !rhs.has_value() ||
        !addAffine(*lhs, *rhs, instruction->opcode() == "add" ? 1 : -1,
                   result)) {
      return std::nullopt;
    }
    expressions[instruction->result()] = result;
  }
  const auto numerator = expressions.find(remainder->operands()[0]);
  if (numerator == expressions.end() || numerator->second.state != 1 ||
      numerator->second.map == 0) {
    return std::nullopt;
  }

  std::int32_t initialState = 0;
  if (!parseI32(candidate.initialState, initialState) ||
      initialState <= -modulus || initialState >= modulus) {
    return std::nullopt;
  }
  const auto mapInterval = intervalForMapRoot(candidate);
  if (!mapInterval.has_value()) {
    return std::nullopt;
  }
  const IntegerInterval normalizedState{-static_cast<std::int64_t>(modulus) + 1,
                                        static_cast<std::int64_t>(modulus) - 1};
  const auto affineRange =
      [&](const AffineIncrement &expression) -> std::optional<IntegerInterval> {
    const auto contribution = [](std::int64_t coefficient,
                                 const IntegerInterval &interval) {
      const __int128 first =
          static_cast<__int128>(coefficient) * interval.minimum;
      const __int128 second =
          static_cast<__int128>(coefficient) * interval.maximum;
      return std::pair<__int128, __int128>{std::min(first, second),
                                           std::max(first, second)};
    };
    const auto stateContribution =
        contribution(expression.state, normalizedState);
    const auto mapContribution = contribution(expression.map, *mapInterval);
    const __int128 minimum =
        stateContribution.first + mapContribution.first + expression.constant;
    const __int128 maximum =
        stateContribution.second + mapContribution.second + expression.constant;
    if (!fitsI32(minimum) || !fitsI32(maximum)) {
      return std::nullopt;
    }
    return IntegerInterval{static_cast<std::int64_t>(minimum),
                           static_cast<std::int64_t>(maximum)};
  };
  for (std::size_t index = 0;
       index + 1 < candidate.recurrenceInstructions.size(); ++index) {
    const auto found =
        expressions.find(candidate.recurrenceInstructions[index]->result());
    if (found == expressions.end() || !affineRange(found->second).has_value()) {
      return std::nullopt;
    }
  }
  const auto coefficient = numerator->second.map;
  const auto constant = numerator->second.constant;
  const __int128 product0 =
      static_cast<__int128>(coefficient) * mapInterval->minimum;
  const __int128 product1 =
      static_cast<__int128>(coefficient) * mapInterval->maximum;
  if (!fitsI32(product0) || !fitsI32(product1)) {
    return std::nullopt;
  }
  const __int128 endpoint0 = product0 + constant;
  const __int128 endpoint1 = product1 + constant;
  const auto minimum128 = std::min(endpoint0, endpoint1);
  const auto maximum128 = std::max(endpoint0, endpoint1);
  if (!fitsI32(minimum128) || !fitsI32(maximum128)) {
    return std::nullopt;
  }
  const IntegerInterval increment{static_cast<std::int64_t>(minimum128),
                                  static_cast<std::int64_t>(maximum128)};
  const auto maximumAbsolute =
      std::max(std::llabs(increment.minimum), std::llabs(increment.maximum));
  const __int128 blockMagnitude =
      static_cast<__int128>(maximumAbsolute) * kPrefixScanWidth;
  if (blockMagnitude >= modulus ||
      static_cast<__int128>(modulus) + blockMagnitude >
          std::numeric_limits<std::int32_t>::max()) {
    return std::nullopt;
  }
  return BoundedRemainderScan{modulus, coefficient, constant, increment};
}

bool isSlotOperand(const VectorOperation &operation, bool lhs, int slot) {
  return lhs ? (!operation.lhsImmediate && operation.lhs == slot)
             : (!operation.rhsImmediate && operation.rhs == slot);
}

bool immediateOperand(const VectorOperation &operation, bool lhs,
                      std::int32_t &value) {
  if (lhs ? !operation.lhsImmediate : !operation.rhsImmediate)
    return false;
  value = lhs ? operation.lhs : operation.rhs;
  return true;
}

std::optional<QuotientBlockSummary>
quotientBlockSummaryFor(const Candidate &candidate,
                        const BoundedRemainderScan &bounded) {
  const auto &operations = candidate.vectorOperations;
  std::size_t index = 0;
  int current = 0;
  std::vector<std::int32_t> mirrors;

  // Consume max(v, C-v) stages.  The first stage supplies the only dynamic
  // piecewise boundary; subsequent stages must be provably redundant over the
  // non-negative fast-path domain.
  while (index + 1 < operations.size()) {
    const auto &subtract = operations[index];
    const auto &maximum = operations[index + 1];
    std::int32_t mirror = 0;
    if (subtract.kind != VectorOperation::Kind::Binary ||
        subtract.opcode != 1 || !immediateOperand(subtract, true, mirror) ||
        !isSlotOperand(subtract, false, current) ||
        maximum.kind != VectorOperation::Kind::Binary || maximum.opcode != 3 ||
        !((isSlotOperand(maximum, true, current) &&
           isSlotOperand(maximum, false, subtract.destination)) ||
          (isSlotOperand(maximum, false, current) &&
           isSlotOperand(maximum, true, subtract.destination)))) {
      break;
    }
    mirrors.push_back(mirror);
    current = maximum.destination;
    index += 2;
  }
  if (mirrors.empty() || mirrors.front() < 0) {
    return std::nullopt;
  }
  const std::int64_t minimumAfterFirst =
      (static_cast<std::int64_t>(mirrors.front()) + 1) / 2;
  for (std::size_t mirrorIndex = 1; mirrorIndex < mirrors.size();
       ++mirrorIndex) {
    if (mirrors[mirrorIndex] < 0 ||
        static_cast<std::int64_t>(mirrors[mirrorIndex]) >
            2 * minimumAfterFirst) {
      return std::nullopt;
    }
  }

  if (index + 5 != operations.size()) {
    return std::nullopt;
  }
  const auto &multiply = operations[index++];
  const auto &divide = operations[index++];
  const auto &scaleQuotient = operations[index++];
  const auto &combine = operations[index++];
  const auto &remainder = operations[index++];
  std::int32_t multiplier = 0;
  if (multiply.kind != VectorOperation::Kind::Binary || multiply.opcode != 2 ||
      !((isSlotOperand(multiply, true, current) &&
         immediateOperand(multiply, false, multiplier)) ||
        (isSlotOperand(multiply, false, current) &&
         immediateOperand(multiply, true, multiplier))) ||
      multiplier == 0 || divide.kind != VectorOperation::Kind::Divide ||
      !isSlotOperand(divide, true, multiply.destination) ||
      divide.divisor <= 1) {
    return std::nullopt;
  }
  std::int32_t quotientMultiplier = 0;
  if (scaleQuotient.kind != VectorOperation::Kind::Binary ||
      scaleQuotient.opcode != 2 ||
      !((isSlotOperand(scaleQuotient, true, divide.destination) &&
         immediateOperand(scaleQuotient, false, quotientMultiplier)) ||
        (isSlotOperand(scaleQuotient, false, divide.destination) &&
         immediateOperand(scaleQuotient, true, quotientMultiplier))) ||
      combine.kind != VectorOperation::Kind::Binary || combine.opcode != 0 ||
      !((isSlotOperand(combine, true, current) &&
         isSlotOperand(combine, false, scaleQuotient.destination)) ||
        (isSlotOperand(combine, false, current) &&
         isSlotOperand(combine, true, scaleQuotient.destination))) ||
      remainder.kind != VectorOperation::Kind::Remainder ||
      !isSlotOperand(remainder, true, combine.destination) ||
      remainder.divisor <= 1 ||
      remainder.destination != candidate.mapRootSlot ||
      bounded.mapCoefficient == 0) {
    return std::nullopt;
  }
  return QuotientBlockSummary{mirrors.front(),        multiplier,
                              divide.divisor,         quotientMultiplier,
                              remainder.divisor,      bounded.modulus,
                              bounded.mapCoefficient, bounded.constant};
}

std::optional<int> selectMinMaxOpcode(const Instruction &select,
                                      const DefinitionMap &definitions,
                                      std::string &lhs, std::string &rhs) {
  if (select.opcode() != "select" || select.operands().size() != 3) {
    return std::nullopt;
  }
  const auto condition = definitions.find(select.operands()[0]);
  if (condition == definitions.end() ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto &compare = *condition->second.instruction;
  lhs = compare.operands()[0];
  rhs = compare.operands()[1];
  const auto &onTrue = select.operands()[1];
  const auto &onFalse = select.operands()[2];
  if (compare.opcode() == "icmp.slt") {
    if (onTrue == rhs && onFalse == lhs)
      return 3;
    if (onTrue == lhs && onFalse == rhs)
      return 4;
  }
  if (compare.opcode() == "icmp.sgt") {
    if (onTrue == lhs && onFalse == rhs)
      return 3;
    if (onTrue == rhs && onFalse == lhs)
      return 4;
  }
  return std::nullopt;
}

bool isSupportedRecurrenceInstruction(const Instruction &instruction) {
  return instruction.opcode() == "add" || instruction.opcode() == "sub" ||
         instruction.opcode() == "mul" || instruction.opcode() == "sdiv" ||
         instruction.opcode() == "srem";
}

std::unordered_set<std::string>
backwardSlice(const std::string &root, const DefinitionMap &definitions,
              const std::string &body,
              const std::unordered_set<std::string> &stops) {
  std::unordered_set<std::string> result;
  std::vector<std::string> pending{root};
  while (!pending.empty()) {
    const auto value = std::move(pending.back());
    pending.pop_back();
    if (stops.find(value) != stops.end() || !result.insert(value).second) {
      continue;
    }
    const auto found = definitions.find(value);
    if (found == definitions.end() || found->second.block != body) {
      continue;
    }
    for (const auto &operand : found->second.instruction->operands()) {
      if (isValue(operand)) {
        pending.push_back(operand);
      }
    }
  }
  return result;
}

std::optional<Candidate> matchCandidate(const Function &function) {
  const auto definitions = definitionsOf(function);
  const auto blocks = blocksOf(function);
  const auto predecessors = predecessorsOf(function);

  for (const auto &header : function.blocks()) {
    const auto &headerInstructions = header.instructions();
    if (headerInstructions.size() != 4 ||
        headerInstructions[0].opcode() != "phi" ||
        headerInstructions[1].opcode() != "phi" ||
        headerInstructions[2].opcode() != "icmp.slt" ||
        headerInstructions[2].operands().size() != 2 ||
        headerInstructions[3].opcode() != "br" ||
        headerInstructions[3].operands().size() != 3 ||
        headerInstructions[3].operands()[0] != headerInstructions[2].result()) {
      continue;
    }
    const std::string bodyName = headerInstructions[3].operands()[1];
    const std::string exitName = headerInstructions[3].operands()[2];
    const auto bodyFound = blocks.find(bodyName);
    const auto predecessorFound = predecessors.find(header.name());
    if (bodyFound == blocks.end() || predecessorFound == predecessors.end() ||
        predecessorFound->second.size() != 2) {
      continue;
    }
    const auto *body = bodyFound->second;
    if (body->terminator() == nullptr || body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    std::string preheaderName;
    for (const auto &predecessor : predecessorFound->second) {
      if (predecessor != bodyName) {
        preheaderName = predecessor;
      }
    }
    const auto preheaderFound = blocks.find(preheaderName);
    if (preheaderName.empty() || preheaderFound == blocks.end() ||
        preheaderFound->second->terminator() == nullptr ||
        preheaderFound->second->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const std::string compared = headerInstructions[2].operands()[0];
    const Instruction *inductionPhi = nullptr;
    const Instruction *statePhi = nullptr;
    for (std::size_t index = 0; index < 2; ++index) {
      if (headerInstructions[index].result() == compared) {
        inductionPhi = &headerInstructions[index];
      } else {
        statePhi = &headerInstructions[index];
      }
    }
    if (inductionPhi == nullptr || statePhi == nullptr) {
      continue;
    }
    const std::string bound = headerInstructions[2].operands()[1];
    if (bound == inductionPhi->result() || bound == statePhi->result()) {
      continue;
    }
    if (isValue(bound)) {
      const auto boundDefinition = definitions.find(bound);
      if (boundDefinition != definitions.end() &&
          (boundDefinition->second.block == header.name() ||
           boundDefinition->second.block == bodyName)) {
        continue;
      }
    }
    const auto inductionBackedge = phiIncoming(*inductionPhi, bodyName);
    const auto stateBackedge = phiIncoming(*statePhi, bodyName);
    const auto initialState = phiIncoming(*statePhi, preheaderName);
    if (!inductionBackedge.has_value() || !stateBackedge.has_value() ||
        !initialState.has_value()) {
      continue;
    }
    const auto inductionUpdate = definitions.find(*inductionBackedge);
    if (inductionUpdate == definitions.end() ||
        inductionUpdate->second.block != bodyName ||
        inductionUpdate->second.instruction->opcode() != "add" ||
        inductionUpdate->second.instruction->operands().size() != 2) {
      continue;
    }
    std::string step;
    if (inductionUpdate->second.instruction->operands()[0] ==
        inductionPhi->result()) {
      step = inductionUpdate->second.instruction->operands()[1];
    } else if (inductionUpdate->second.instruction->operands()[1] ==
               inductionPhi->result()) {
      step = inductionUpdate->second.instruction->operands()[0];
    } else {
      continue;
    }
    if (step == inductionPhi->result() || step == statePhi->result()) {
      continue;
    }
    if (isValue(step)) {
      const auto stepDefinition = definitions.find(step);
      if (stepDefinition != definitions.end() &&
          (stepDefinition->second.block == header.name() ||
           stepDefinition->second.block == bodyName)) {
        continue;
      }
    }

    std::unordered_map<std::string, Dependency> dependencies;
    dependencies.emplace(statePhi->result(), Dependency{true, false});
    dependencies.emplace(inductionPhi->result(), Dependency{false, true});
    bool dependencyFailure = false;
    for (const auto &instruction : body->instructions()) {
      if (!instruction.hasResult()) {
        continue;
      }
      Dependency dependency;
      for (const auto &operand : instruction.operands()) {
        const auto found = dependencies.find(operand);
        if (found != dependencies.end()) {
          dependency.state = dependency.state || found->second.state;
          dependency.induction =
              dependency.induction || found->second.induction;
        } else if (isValue(operand)) {
          const auto definition = definitions.find(operand);
          if (definition != definitions.end() &&
              definition->second.block == bodyName) {
            dependencyFailure = true;
          }
        }
      }
      dependencies[instruction.result()] = dependency;
    }
    if (dependencyFailure) {
      continue;
    }
    const auto finalDependency = dependencies.find(*stateBackedge);
    if (finalDependency == dependencies.end() ||
        !finalDependency->second.state || !finalDependency->second.induction) {
      continue;
    }

    std::unordered_set<std::string> recurrenceSlice;
    std::unordered_set<std::string> mapRoots;
    bool recurrenceSupported = true;
    std::vector<std::string> recurrencePending{*stateBackedge};
    while (!recurrencePending.empty()) {
      const auto value = std::move(recurrencePending.back());
      recurrencePending.pop_back();
      if (value == statePhi->result()) {
        continue;
      }
      const auto dependencyFound = dependencies.find(value);
      if (dependencyFound != dependencies.end() &&
          dependencyFound->second.induction && !dependencyFound->second.state) {
        mapRoots.insert(value);
        continue;
      }
      if (!recurrenceSlice.insert(value).second) {
        continue;
      }
      const auto found = definitions.find(value);
      if (found == definitions.end() || found->second.block != bodyName) {
        continue;
      }
      if (!isSupportedRecurrenceInstruction(*found->second.instruction)) {
        recurrenceSupported = false;
      }
      for (const auto &operand : found->second.instruction->operands()) {
        if (isValue(operand)) {
          recurrencePending.push_back(operand);
        }
      }
    }
    if (!recurrenceSupported || mapRoots.size() != 1) {
      continue;
    }
    const std::string mapRoot = *mapRoots.begin();
    const auto mapSlice =
        backwardSlice(mapRoot, definitions, bodyName, {inductionPhi->result()});

    std::unordered_map<std::string, int> slots;
    slots.emplace(inductionPhi->result(), 0);
    std::vector<VectorOperation> operations;
    int nextSlot = 1;
    bool mapSupported = true;
    const auto vectorOperand = [&](const std::string &operand, bool &immediate,
                                   std::int32_t &encoded) -> bool {
      const auto slot = slots.find(operand);
      if (slot != slots.end()) {
        immediate = false;
        encoded = slot->second;
        return true;
      }
      immediate = true;
      return parseI32(operand, encoded);
    };

    for (const auto &instruction : body->instructions()) {
      if (!instruction.hasResult() ||
          mapSlice.find(instruction.result()) == mapSlice.end()) {
        continue;
      }
      if (instruction.opcode().rfind("icmp.", 0) == 0) {
        continue;
      }
      if (nextSlot >= kMaximumVectorSlots) {
        mapSupported = false;
        break;
      }
      VectorOperation operation;
      operation.destination = nextSlot++;
      if (const auto opcode = binaryOpcode(instruction.opcode());
          opcode.has_value() && instruction.operands().size() == 2) {
        operation.kind = VectorOperation::Kind::Binary;
        operation.opcode = *opcode;
        if (!vectorOperand(instruction.operands()[0], operation.lhsImmediate,
                           operation.lhs) ||
            !vectorOperand(instruction.operands()[1], operation.rhsImmediate,
                           operation.rhs)) {
          mapSupported = false;
          break;
        }
      } else if (instruction.opcode() == "select") {
        std::string lhs;
        std::string rhs;
        const auto opcode =
            selectMinMaxOpcode(instruction, definitions, lhs, rhs);
        if (!opcode.has_value()) {
          mapSupported = false;
          break;
        }
        operation.kind = VectorOperation::Kind::Binary;
        operation.opcode = *opcode;
        if (!vectorOperand(lhs, operation.lhsImmediate, operation.lhs) ||
            !vectorOperand(rhs, operation.rhsImmediate, operation.rhs)) {
          mapSupported = false;
          break;
        }
      } else if ((instruction.opcode() == "sdiv" ||
                  instruction.opcode() == "srem") &&
                 instruction.operands().size() == 2) {
        std::int32_t divisor = 0;
        if (!parseI32(instruction.operands()[1], divisor) || divisor <= 1 ||
            !vectorOperand(instruction.operands()[0], operation.lhsImmediate,
                           operation.lhs) ||
            operation.lhsImmediate) {
          mapSupported = false;
          break;
        }
        operation.kind = instruction.opcode() == "sdiv"
                             ? VectorOperation::Kind::Divide
                             : VectorOperation::Kind::Remainder;
        operation.divisor = divisor;
        operation.magic =
            signedMagicForPositiveDivisor(static_cast<std::uint32_t>(divisor));
      } else {
        mapSupported = false;
        break;
      }
      slots.emplace(instruction.result(), operation.destination);
      operations.push_back(operation);
    }
    const auto rootSlot = slots.find(mapRoot);
    if (!mapSupported || rootSlot == slots.end() || operations.empty()) {
      continue;
    }

    std::unordered_set<std::string> allowedResults{*inductionBackedge};
    allowedResults.insert(mapSlice.begin(), mapSlice.end());
    allowedResults.insert(recurrenceSlice.begin(), recurrenceSlice.end());
    bool exactBody = true;
    for (const auto &instruction : body->instructions()) {
      if (instruction.isTerminator()) {
        continue;
      }
      if (!instruction.hasResult() ||
          allowedResults.find(instruction.result()) == allowedResults.end()) {
        exactBody = false;
        break;
      }
    }
    if (!exactBody) {
      continue;
    }

    std::vector<const Instruction *> recurrenceInstructions;
    for (const auto &instruction : body->instructions()) {
      if (instruction.hasResult() &&
          recurrenceSlice.find(instruction.result()) != recurrenceSlice.end() &&
          mapSlice.find(instruction.result()) == mapSlice.end()) {
        recurrenceInstructions.push_back(&instruction);
      }
    }
    if (recurrenceInstructions.empty() ||
        recurrenceInstructions.back()->result() != *stateBackedge) {
      continue;
    }

    return Candidate{header.name(),
                     bodyName,
                     preheaderName,
                     exitName,
                     inductionPhi->result(),
                     statePhi->result(),
                     *inductionBackedge,
                     *stateBackedge,
                     *initialState,
                     step,
                     mapRoot,
                     rootSlot->second,
                     std::move(operations),
                     std::move(recurrenceInstructions)};
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectValueNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

BasicBlock *blockByName(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

Instruction appendPhiIncoming(const Instruction &phi, const std::string &value,
                              const std::string &predecessor) {
  auto operands = phi.operands();
  operands.push_back(value);
  operands.push_back(predecessor);
  return Instruction{"phi", std::move(operands), phi.result()};
}

bool transform(Function &function, const Candidate &candidate) {
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto boundedScan =
      std::getenv("COMPILER_DISABLE_BOUNDED_REMAINDER_PREFIX_SCAN") == nullptr
          ? boundedRemainderScanFor(candidate)
          : std::nullopt;
  const auto quotientSummary =
      boundedScan.has_value() &&
              std::getenv("COMPILER_DISABLE_AFFINE_QUOTIENT_BLOCK_SUMMARY") ==
                  nullptr
          ? quotientBlockSummaryFor(candidate, *boundedScan)
          : std::nullopt;
  auto *header = blockByName(function, candidate.header);
  if (header == nullptr || header->instructions().size() != 4) {
    return false;
  }
  const auto scanGuardName = freshName(blockNames, "pure.map.prefix32.guard");
  const auto quotientGuardName =
      freshName(blockNames, "pure.map.quotient.guard");
  const auto quotientBodyName = freshName(blockNames, "pure.map.quotient.body");
  const auto quotientCommitName =
      freshName(blockNames, "pure.map.quotient.commit");
  const auto scanHeaderName = freshName(blockNames, "pure.map.prefix32.header");
  const auto scanStepName = freshName(blockNames, "pure.map.prefix32.step");
  const auto scanFinishName = freshName(blockNames, "pure.map.prefix32.finish");
  const auto guardName = freshName(blockNames, "pure.map.vector.guard");
  const auto vectorName = freshName(blockNames, "pure.map.vector.body");
  const auto x1 = freshName(valueNames, "%pure.map.x1");
  const auto x2 = freshName(valueNames, "%pure.map.x2");
  const auto x3 = freshName(valueNames, "%pure.map.x3");
  const auto x4 = freshName(valueNames, "%pure.map.x4");
  const auto c1 = freshName(valueNames, "%pure.map.c1");
  const auto c2 = freshName(valueNames, "%pure.map.c2");
  const auto c3 = freshName(valueNames, "%pure.map.c3");
  const auto c12 = freshName(valueNames, "%pure.map.c12");
  const auto all = freshName(valueNames, "%pure.map.all");

  header->instructions().back() =
      Instruction{"br",
                  {header->instructions()[2].result(),
                   quotientSummary.has_value()
                       ? quotientGuardName
                       : (boundedScan.has_value() ? scanGuardName : guardName),
                   candidate.exit}};

  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"add", {candidate.induction, candidate.step}, x1});
  guard.addInstruction(Instruction{"add", {x1, candidate.step}, x2});
  guard.addInstruction(Instruction{"add", {x2, candidate.step}, x3});
  guard.addInstruction(Instruction{
      "icmp.slt", {x1, header->instructions()[2].operands()[1]}, c1});
  guard.addInstruction(Instruction{
      "icmp.slt", {x2, header->instructions()[2].operands()[1]}, c2});
  guard.addInstruction(Instruction{
      "icmp.slt", {x3, header->instructions()[2].operands()[1]}, c3});
  guard.addInstruction(Instruction{"and", {c1, c2}, c12});
  guard.addInstruction(Instruction{"and", {c12, c3}, all});
  guard.addInstruction(Instruction{"br", {all, vectorName, candidate.body}});

  const auto appendVectorMap = [&](BasicBlock &block,
                                   const std::array<std::string, 4> &inputs) {
    block.addInstruction(
        Instruction{"aarch64.neon.pure.map.i32x4.init",
                    {inputs[0], inputs[1], inputs[2], inputs[3]}});
    for (const auto &operation : candidate.vectorOperations) {
      if (operation.kind == VectorOperation::Kind::Binary) {
        block.addInstruction(Instruction{"aarch64.neon.pure.map.binary.i32x4",
                                         {std::to_string(operation.opcode),
                                          std::to_string(operation.destination),
                                          operation.lhsImmediate ? "1" : "0",
                                          std::to_string(operation.lhs),
                                          operation.rhsImmediate ? "1" : "0",
                                          std::to_string(operation.rhs)}});
      } else {
        block.addInstruction(Instruction{
            operation.kind == VectorOperation::Kind::Divide
                ? "aarch64.neon.pure.map.sdiv.i32x4"
                : "aarch64.neon.pure.map.srem.i32x4",
            {std::to_string(operation.destination),
             std::to_string(operation.lhs), std::to_string(operation.divisor),
             std::to_string(operation.magic.multiplier),
             std::to_string(operation.magic.shift)}});
      }
    }
    std::array<std::string, 4> lanes;
    for (int lane = 0; lane < kVectorWidth; ++lane) {
      lanes[static_cast<std::size_t>(lane)] =
          freshName(valueNames, "%pure.map.lane");
      block.addInstruction(Instruction{
          "aarch64.neon.pure.map.extract.i32x4",
          {std::to_string(candidate.mapRootSlot), std::to_string(lane)},
          lanes[static_cast<std::size_t>(lane)]});
    }
    return lanes;
  };

  BasicBlock vectorBody{vectorName};
  const auto laneValues =
      appendVectorMap(vectorBody, {candidate.induction, x1, x2, x3});

  std::string currentState = candidate.state;
  for (int lane = 0; lane < kVectorWidth; ++lane) {
    std::unordered_map<std::string, std::string> replacements{
        {candidate.state, currentState}, {candidate.mapRoot, laneValues[lane]}};
    for (const auto *instruction : candidate.recurrenceInstructions) {
      auto operands = instruction->operands();
      for (auto &operand : operands) {
        const auto found = replacements.find(operand);
        if (found != replacements.end()) {
          operand = found->second;
        }
      }
      const auto result = freshName(valueNames, "%pure.map.recurrence");
      replacements[instruction->result()] = result;
      vectorBody.addInstruction(
          Instruction{instruction->opcode(), std::move(operands), result});
    }
    currentState = replacements.at(candidate.stateBackedge);
  }
  vectorBody.addInstruction(Instruction{"add", {x3, candidate.step}, x4});
  vectorBody.addInstruction(Instruction{"br", {candidate.header}});

  BasicBlock quotientGuard{quotientGuardName};
  BasicBlock quotientBody{quotientBodyName};
  BasicBlock quotientCommit{quotientCommitName};
  std::string quotientNextInduction;
  std::string quotientNextState;
  if (quotientSummary.has_value()) {
    const auto qName = [&](const std::string &stem) {
      return freshName(valueNames, "%pure.map.quotient." + stem);
    };
    const auto add = [&](BasicBlock &block, const std::string &opcode,
                         std::vector<std::string> operands,
                         const std::string &stem) {
      const auto result = qName(stem);
      block.addInstruction(Instruction{opcode, std::move(operands), result});
      return result;
    };
    const auto wrapI32 = [&](BasicBlock &block, const std::string &value,
                             const std::string &stem) {
      const auto narrowed = add(block, "trunc", {value, "int"}, stem + ".i32");
      return add(block, "sext", {narrowed, "long"}, stem + ".i64");
    };
    const auto minimum = [&](BasicBlock &block, const std::string &lhs,
                             const std::string &rhs, const std::string &stem) {
      const auto less = add(block, "icmp.slt", {lhs, rhs}, stem + ".less");
      return add(block, "select", {less, lhs, rhs}, stem + ".value");
    };
    const auto quotientRun = [&](BasicBlock &block, const std::string &value,
                                 const std::string &delta, std::int32_t divisor,
                                 const std::string &stem) {
      const auto divisorText = std::to_string(divisor);
      const auto quotient =
          add(block, "sdiv", {value, divisorText}, stem + ".quotient");
      const auto nonnegative =
          add(block, "icmp.sge", {quotient, "0"}, stem + ".nonnegative");
      const auto qTimes =
          add(block, "mul", {quotient, divisorText}, stem + ".q.times");
      const auto qPlusOne =
          add(block, "add", {quotient, "1"}, stem + ".q.plus.one");
      const auto positiveHighProduct =
          add(block, "mul", {qPlusOne, divisorText},
              stem + ".positive.high.product");
      const auto positiveHigh = add(block, "sub", {positiveHighProduct, "1"},
                                    stem + ".positive.high");
      const auto qMinusOne =
          add(block, "sub", {quotient, "1"}, stem + ".q.minus.one");
      const auto negativeLowProduct =
          add(block, "mul", {qMinusOne, divisorText},
              stem + ".negative.low.product");
      const auto negativeLow =
          add(block, "add", {negativeLowProduct, "1"}, stem + ".negative.low");
      const auto high = add(
          block, "select", {nonnegative, positiveHigh, qTimes}, stem + ".high");
      const auto low = add(block, "select", {nonnegative, qTimes, negativeLow},
                           stem + ".low");
      const auto deltaPositive =
          add(block, "icmp.sgt", {delta, "0"}, stem + ".delta.positive");
      const auto negativeDelta =
          add(block, "sub", {"0", delta}, stem + ".negative.delta");
      const auto absoluteDelta =
          add(block, "select", {deltaPositive, delta, negativeDelta},
              stem + ".absolute.delta");
      const auto upwardDistance =
          add(block, "sub", {high, value}, stem + ".upward.distance");
      const auto downwardDistance =
          add(block, "sub", {value, low}, stem + ".downward.distance");
      const auto distance = add(
          block, "select", {deltaPositive, upwardDistance, downwardDistance},
          stem + ".distance");
      const auto completeSteps =
          add(block, "sdiv", {distance, absoluteDelta}, stem + ".steps");
      return std::pair{quotient,
                       add(block, "add", {completeSteps, "1"}, stem + ".run")};
    };
    const auto wrapRun = [&](BasicBlock &block, const std::string &value,
                             const std::string &delta,
                             const std::string &stem) {
      const auto deltaPositive =
          add(block, "icmp.sgt", {delta, "0"}, stem + ".delta.positive");
      const auto negativeDelta =
          add(block, "sub", {"0", delta}, stem + ".negative.delta");
      const auto absoluteDelta =
          add(block, "select", {deltaPositive, delta, negativeDelta},
              stem + ".absolute.delta");
      const auto upward =
          add(block, "sub", {"2147483647", value}, stem + ".upward");
      const auto downward =
          add(block, "sub", {value, "-2147483648"}, stem + ".downward");
      const auto distance =
          add(block, "select", {deltaPositive, upward, downward},
              stem + ".distance");
      const auto completeSteps =
          add(block, "sdiv", {distance, absoluteDelta}, stem + ".steps");
      return add(block, "add", {completeSteps, "1"}, stem + ".run");
    };

    const auto induction64 = add(quotientGuard, "sext",
                                 {candidate.induction, "long"}, "induction64");
    const auto step64 =
        add(quotientGuard, "sext", {candidate.step, "long"}, "step64");
    const auto bound64 =
        add(quotientGuard, "sext",
            {header->instructions()[2].operands()[1], "long"}, "bound64");
    const auto nonnegativeInduction = add(
        quotientGuard, "icmp.sge", {induction64, "0"}, "induction.nonnegative");
    const auto positiveStep =
        add(quotientGuard, "icmp.sgt", {step64, "0"}, "step.positive");
    const auto difference =
        add(quotientGuard, "sub", {bound64, induction64}, "difference");
    const auto stepMinusOne =
        add(quotientGuard, "sub", {step64, "1"}, "step.minus.one");
    const auto roundedDifference = add(
        quotientGuard, "add", {difference, stepMinusOne}, "difference.rounded");
    const auto remaining =
        add(quotientGuard, "sdiv", {roundedDifference, step64}, "remaining");
    const auto fullAdvance =
        add(quotientGuard, "mul", {remaining, step64}, "full.advance");
    const auto finalInduction = add(
        quotientGuard, "add", {induction64, fullAdvance}, "final.induction");
    const auto finalFits = add(quotientGuard, "icmp.sle",
                               {finalInduction, "2147483647"}, "final.fits");
    const auto domainValid =
        add(quotientGuard, "and", {nonnegativeInduction, positiveStep},
            "domain.valid");
    const auto summaryValid =
        add(quotientGuard, "and", {domainValid, finalFits}, "summary.valid");
    quotientGuard.addInstruction(
        Instruction{"br",
                    {summaryValid, quotientBodyName,
                     boundedScan.has_value() ? scanGuardName : guardName}});

    const auto mirrorValue = add(
        quotientBody, "sub",
        {std::to_string(quotientSummary->mirror), induction64}, "mirror.value");
    const auto useMirror =
        add(quotientBody, "icmp.slt", {induction64, mirrorValue}, "use.mirror");
    const auto mappedBase =
        add(quotientBody, "select", {useMirror, mirrorValue, induction64},
            "mapped.base");
    const auto twiceInduction =
        add(quotientBody, "mul", {induction64, "2"}, "twice.induction");
    const auto mirrorNumerator0 =
        add(quotientBody, "sub", {std::to_string(quotientSummary->mirror), "1"},
            "mirror.numerator.base");
    const auto mirrorNumerator =
        add(quotientBody, "sub", {mirrorNumerator0, twiceInduction},
            "mirror.numerator");
    const auto twiceStep =
        add(quotientBody, "mul", {step64, "2"}, "twice.step");
    const auto mirrorSteps =
        add(quotientBody, "sdiv", {mirrorNumerator, twiceStep}, "mirror.steps");
    const auto mirrorRun =
        add(quotientBody, "add", {mirrorSteps, "1"}, "mirror.run");
    const auto branchRun = add(quotientBody, "select",
                               {useMirror, mirrorRun, remaining}, "branch.run");

    const auto rawFull = add(
        quotientBody, "mul",
        {mappedBase, std::to_string(quotientSummary->multiplier)}, "raw.full");
    const auto raw = wrapI32(quotientBody, rawFull, "raw");
    const auto negativeStep =
        add(quotientBody, "sub", {"0", step64}, "negative.step");
    const auto mappedDelta =
        add(quotientBody, "select", {useMirror, negativeStep, step64},
            "mapped.delta");
    const auto rawDelta =
        add(quotientBody, "mul",
            {mappedDelta, std::to_string(quotientSummary->multiplier)},
            "raw.delta");
    const auto rawQuotientAndRun = quotientRun(
        quotientBody, raw, rawDelta, quotientSummary->divisor, "division");
    const auto rawWrapRun = wrapRun(quotientBody, raw, rawDelta, "raw.wrap");

    const auto quotientProduct =
        add(quotientBody, "mul",
            {rawQuotientAndRun.first,
             std::to_string(quotientSummary->quotientMultiplier)},
            "quotient.product");
    const auto wrappedQuotientProduct =
        wrapI32(quotientBody, quotientProduct, "quotient.product");
    const auto combinedFull =
        add(quotientBody, "add", {mappedBase, wrappedQuotientProduct},
            "combined.full");
    const auto combined = wrapI32(quotientBody, combinedFull, "combined");
    const auto mapRemainderAndRun =
        quotientRun(quotientBody, combined, mappedDelta,
                    quotientSummary->mapModulus, "remainder");
    const auto combinedWrapRun =
        wrapRun(quotientBody, combined, mappedDelta, "combined.wrap");
    const auto remainderMultiple = add(
        quotientBody, "mul",
        {mapRemainderAndRun.first, std::to_string(quotientSummary->mapModulus)},
        "remainder.multiple");
    const auto mapValue =
        add(quotientBody, "sub", {combined, remainderMultiple}, "map.value");
    auto increment = mapValue;
    if (quotientSummary->mapCoefficient != 1) {
      increment =
          add(quotientBody, "mul",
              {increment, std::to_string(quotientSummary->mapCoefficient)},
              "increment.scaled");
    }
    if (quotientSummary->incrementConstant != 0) {
      increment =
          add(quotientBody, "add",
              {increment, std::to_string(quotientSummary->incrementConstant)},
              "increment.shifted");
    }
    const auto incrementDelta =
        add(quotientBody, "mul",
            {mappedDelta, std::to_string(quotientSummary->mapCoefficient)},
            "increment.delta");
    const auto incrementNonnegative = add(
        quotientBody, "icmp.sge", {increment, "0"}, "increment.nonnegative");
    const auto deltaNegative =
        add(quotientBody, "icmp.slt", {incrementDelta, "0"}, "delta.negative");
    const auto deltaPositive =
        add(quotientBody, "icmp.sgt", {incrementDelta, "0"}, "delta.positive");
    const auto positiveTowardZero =
        add(quotientBody, "and", {incrementNonnegative, deltaNegative},
            "positive.toward.zero");
    const auto incrementNegative =
        add(quotientBody, "icmp.slt", {increment, "0"}, "increment.negative");
    const auto negativeTowardZero =
        add(quotientBody, "and", {incrementNegative, deltaPositive},
            "negative.toward.zero");
    const auto negativeIncrementDelta = add(
        quotientBody, "sub", {"0", incrementDelta}, "negative.increment.delta");
    const auto positiveSignSteps =
        add(quotientBody, "sdiv", {increment, negativeIncrementDelta},
            "positive.sign.steps");
    const auto positiveSignRun =
        add(quotientBody, "add", {positiveSignSteps, "1"}, "positive.sign.run");
    const auto negativeIncrement =
        add(quotientBody, "sub", {"0", increment}, "negative.increment");
    const auto negativeSignSteps =
        add(quotientBody, "sdiv", {negativeIncrement, incrementDelta},
            "negative.sign.steps");
    const auto negativeSignRun =
        add(quotientBody, "add", {negativeSignSteps, "1"}, "negative.sign.run");
    const auto signRun0 = add(quotientBody, "select",
                              {positiveTowardZero, positiveSignRun, remaining},
                              "sign.run.positive");
    const auto signRun =
        add(quotientBody, "select",
            {negativeTowardZero, negativeSignRun, signRun0}, "sign.run");

    auto run = minimum(quotientBody, remaining, branchRun, "run.branch");
    run = minimum(quotientBody, run, rawQuotientAndRun.second, "run.division");
    run = minimum(quotientBody, run, rawWrapRun, "run.raw.wrap");
    run =
        minimum(quotientBody, run, mapRemainderAndRun.second, "run.remainder");
    run = minimum(quotientBody, run, combinedWrapRun, "run.combined.wrap");
    run = minimum(quotientBody, run, signRun, "run.sign");
    run = minimum(quotientBody, run, "1048576", "run.budget");
    const auto runPositive =
        add(quotientBody, "icmp.sgt", {run, "0"}, "run.positive");
    quotientBody.addInstruction(
        Instruction{"br", {runPositive, quotientCommitName, candidate.body}});

    const auto runMinusOne =
        add(quotientCommit, "sub", {run, "1"}, "run.minus.one");
    const auto pairProduct =
        add(quotientCommit, "mul", {run, runMinusOne}, "pair.product");
    const auto pairCount =
        add(quotientCommit, "sdiv", {pairProduct, "2"}, "pair.count");
    const auto baseSum =
        add(quotientCommit, "mul", {run, increment}, "base.sum");
    const auto deltaSum =
        add(quotientCommit, "mul", {pairCount, incrementDelta}, "delta.sum");
    const auto incrementSum =
        add(quotientCommit, "add", {baseSum, deltaSum}, "increment.sum");
    const auto state64 =
        add(quotientCommit, "sext", {candidate.state, "long"}, "state64");
    const auto stateTotal =
        add(quotientCommit, "add", {state64, incrementSum}, "state.total");
    const auto stateRemainder =
        add(quotientCommit, "srem",
            {stateTotal, std::to_string(quotientSummary->stateModulus)},
            "state.remainder");
    quotientNextState =
        add(quotientCommit, "trunc", {stateRemainder, "int"}, "state.next");
    const auto inductionAdvance =
        add(quotientCommit, "mul", {run, step64}, "induction.advance");
    const auto nextInduction64 =
        add(quotientCommit, "add", {induction64, inductionAdvance},
            "induction.next64");
    quotientNextInduction = add(quotientCommit, "trunc",
                                {nextInduction64, "int"}, "induction.next");
    quotientCommit.addInstruction(Instruction{"br", {candidate.header}});
  }

  BasicBlock scanGuard{scanGuardName};
  BasicBlock scanHeader{scanHeaderName};
  BasicBlock scanStep{scanStepName};
  BasicBlock scanFinish{scanFinishName};
  std::string scanNextInduction;
  std::string scanNextState;
  if (boundedScan.has_value()) {
    const auto induction64 =
        freshName(valueNames, "%pure.map.prefix32.induction64");
    const auto step64 = freshName(valueNames, "%pure.map.prefix32.step64");
    const auto bound64 = freshName(valueNames, "%pure.map.prefix32.bound64");
    const auto stepSpan64 =
        freshName(valueNames, "%pure.map.prefix32.step.span64");
    const auto last64 = freshName(valueNames, "%pure.map.prefix32.last64");
    const auto stepPositive =
        freshName(valueNames, "%pure.map.prefix32.step.positive");
    const auto lastFits = freshName(valueNames, "%pure.map.prefix32.last.fits");
    const auto lastBeforeBound =
        freshName(valueNames, "%pure.map.prefix32.last.before.bound");
    const auto validStepAndRange =
        freshName(valueNames, "%pure.map.prefix32.step.and.range");
    const auto scanAllowed =
        freshName(valueNames, "%pure.map.prefix32.allowed");
    scanGuard.addInstruction(
        Instruction{"sext", {candidate.induction, "long"}, induction64});
    scanGuard.addInstruction(
        Instruction{"sext", {candidate.step, "long"}, step64});
    scanGuard.addInstruction(Instruction{
        "sext", {header->instructions()[2].operands()[1], "long"}, bound64});
    scanGuard.addInstruction(Instruction{"mul", {step64, "31"}, stepSpan64});
    scanGuard.addInstruction(
        Instruction{"add", {induction64, stepSpan64}, last64});
    scanGuard.addInstruction(
        Instruction{"icmp.sgt", {step64, "0"}, stepPositive});
    scanGuard.addInstruction(
        Instruction{"icmp.sle", {last64, "2147483647"}, lastFits});
    scanGuard.addInstruction(
        Instruction{"icmp.slt", {last64, bound64}, lastBeforeBound});
    scanGuard.addInstruction(
        Instruction{"and", {stepPositive, lastFits}, validStepAndRange});
    scanGuard.addInstruction(
        Instruction{"and", {validStepAndRange, lastBeforeBound}, scanAllowed});
    scanGuard.addInstruction(
        Instruction{"br", {scanAllowed, scanHeaderName, guardName}});

    const auto group = freshName(valueNames, "%pure.map.prefix32.group");
    const auto groupNext =
        freshName(valueNames, "%pure.map.prefix32.group.next");
    const auto currentInduction =
        freshName(valueNames, "%pure.map.prefix32.induction.current");
    const auto loopNextInduction =
        freshName(valueNames, "%pure.map.prefix32.induction.next");
    scanNextInduction = currentInduction;
    const auto total = freshName(valueNames, "%pure.map.prefix32.total");
    const auto totalNext =
        freshName(valueNames, "%pure.map.prefix32.total.next");
    const auto minimum = freshName(valueNames, "%pure.map.prefix32.minimum");
    const auto minimumNext =
        freshName(valueNames, "%pure.map.prefix32.minimum.next");
    const auto maximum = freshName(valueNames, "%pure.map.prefix32.maximum");
    const auto maximumNext =
        freshName(valueNames, "%pure.map.prefix32.maximum.next");
    const auto hasGroup = freshName(valueNames, "%pure.map.prefix32.has.group");
    scanHeader.addInstruction(Instruction{
        "phi", {"int", "0", scanGuardName, groupNext, scanStepName}, group});
    scanHeader.addInstruction(
        Instruction{"phi",
                    {"int", candidate.induction, scanGuardName,
                     loopNextInduction, scanStepName},
                    currentInduction});
    scanHeader.addInstruction(Instruction{
        "phi", {"int", "0", scanGuardName, totalNext, scanStepName}, total});
    scanHeader.addInstruction(
        Instruction{"phi",
                    {"int", "0", scanGuardName, minimumNext, scanStepName},
                    minimum});
    scanHeader.addInstruction(
        Instruction{"phi",
                    {"int", "0", scanGuardName, maximumNext, scanStepName},
                    maximum});
    scanHeader.addInstruction(Instruction{"icmp.slt", {group, "8"}, hasGroup});
    scanHeader.addInstruction(
        Instruction{"br", {hasGroup, scanStepName, scanFinishName}});

    std::array<std::string, 4> inputs;
    inputs[0] = currentInduction;
    for (int lane = 1; lane < kVectorWidth; ++lane) {
      inputs[static_cast<std::size_t>(lane)] =
          freshName(valueNames, "%pure.map.prefix32.induction");
      scanStep.addInstruction(Instruction{
          "add",
          {inputs[static_cast<std::size_t>(lane - 1)], candidate.step},
          inputs[static_cast<std::size_t>(lane)]});
    }
    const auto lanes = appendVectorMap(scanStep, inputs);
    std::string runningTotal = total;
    std::string runningMinimum = minimum;
    std::string runningMaximum = maximum;
    for (int lane = 0; lane < kVectorWidth; ++lane) {
      std::string increment = lanes[static_cast<std::size_t>(lane)];
      if (boundedScan->mapCoefficient != 1) {
        const auto scaled = freshName(valueNames, "%pure.map.prefix32.scaled");
        scanStep.addInstruction(Instruction{
            "mul",
            {increment, std::to_string(boundedScan->mapCoefficient)},
            scaled});
        increment = scaled;
      }
      if (boundedScan->constant != 0) {
        const auto shifted =
            freshName(valueNames, "%pure.map.prefix32.shifted");
        scanStep.addInstruction(
            Instruction{"add",
                        {increment, std::to_string(boundedScan->constant)},
                        shifted});
        increment = shifted;
      }
      const auto nextTotal =
          lane + 1 == kVectorWidth
              ? totalNext
              : freshName(valueNames, "%pure.map.prefix32.partial.total");
      const auto belowMinimum =
          freshName(valueNames, "%pure.map.prefix32.below.minimum");
      const auto nextMinimum =
          lane + 1 == kVectorWidth
              ? minimumNext
              : freshName(valueNames, "%pure.map.prefix32.partial.minimum");
      const auto aboveMaximum =
          freshName(valueNames, "%pure.map.prefix32.above.maximum");
      const auto nextMaximum =
          lane + 1 == kVectorWidth
              ? maximumNext
              : freshName(valueNames, "%pure.map.prefix32.partial.maximum");
      scanStep.addInstruction(
          Instruction{"add", {runningTotal, increment}, nextTotal});
      scanStep.addInstruction(
          Instruction{"icmp.slt", {nextTotal, runningMinimum}, belowMinimum});
      scanStep.addInstruction(Instruction{
          "select", {belowMinimum, nextTotal, runningMinimum}, nextMinimum});
      scanStep.addInstruction(
          Instruction{"icmp.sgt", {nextTotal, runningMaximum}, aboveMaximum});
      scanStep.addInstruction(Instruction{
          "select", {aboveMaximum, nextTotal, runningMaximum}, nextMaximum});
      runningTotal = nextTotal;
      runningMinimum = nextMinimum;
      runningMaximum = nextMaximum;
    }
    scanStep.addInstruction(
        Instruction{"add", {inputs[3], candidate.step}, loopNextInduction});
    scanStep.addInstruction(Instruction{"add", {group, "1"}, groupNext});
    scanStep.addInstruction(Instruction{"br", {scanHeaderName}});

    const auto stateWithTotal =
        freshName(valueNames, "%pure.map.prefix32.state.total");
    const auto stateWithMinimum =
        freshName(valueNames, "%pure.map.prefix32.state.minimum");
    const auto stateWithMaximum =
        freshName(valueNames, "%pure.map.prefix32.state.maximum");
    const auto crossesUpper =
        freshName(valueNames, "%pure.map.prefix32.crosses.upper");
    const auto crossesLower =
        freshName(valueNames, "%pure.map.prefix32.crosses.lower");
    const auto upperAdjusted =
        freshName(valueNames, "%pure.map.prefix32.upper.adjusted");
    const auto lowerAdjusted =
        freshName(valueNames, "%pure.map.prefix32.lower.adjusted");
    const auto lowerOrBase =
        freshName(valueNames, "%pure.map.prefix32.lower.or.base");
    scanNextState = freshName(valueNames, "%pure.map.prefix32.state.next");
    scanFinish.addInstruction(
        Instruction{"add", {candidate.state, total}, stateWithTotal});
    scanFinish.addInstruction(
        Instruction{"add", {candidate.state, minimum}, stateWithMinimum});
    scanFinish.addInstruction(
        Instruction{"add", {candidate.state, maximum}, stateWithMaximum});
    scanFinish.addInstruction(
        Instruction{"icmp.sge",
                    {stateWithMaximum, std::to_string(boundedScan->modulus)},
                    crossesUpper});
    scanFinish.addInstruction(
        Instruction{"icmp.sle",
                    {stateWithMinimum, std::to_string(-boundedScan->modulus)},
                    crossesLower});
    scanFinish.addInstruction(
        Instruction{"sub",
                    {stateWithTotal, std::to_string(boundedScan->modulus)},
                    upperAdjusted});
    scanFinish.addInstruction(
        Instruction{"add",
                    {stateWithTotal, std::to_string(boundedScan->modulus)},
                    lowerAdjusted});
    scanFinish.addInstruction(Instruction{
        "select", {crossesLower, lowerAdjusted, stateWithTotal}, lowerOrBase});
    scanFinish.addInstruction(Instruction{
        "select", {crossesUpper, upperAdjusted, lowerOrBase}, scanNextState});
    scanFinish.addInstruction(Instruction{"br", {candidate.header}});
  }

  for (std::size_t index = 0; index < 2; ++index) {
    const auto &phi = header->instructions()[index];
    if (phi.result() == candidate.induction) {
      auto updated = appendPhiIncoming(phi, x4, vectorName);
      if (boundedScan.has_value()) {
        updated = appendPhiIncoming(updated, scanNextInduction, scanFinishName);
      }
      if (quotientSummary.has_value()) {
        updated = appendPhiIncoming(updated, quotientNextInduction,
                                    quotientCommitName);
      }
      header->instructions()[index] = std::move(updated);
    } else if (phi.result() == candidate.state) {
      auto updated = appendPhiIncoming(phi, currentState, vectorName);
      if (boundedScan.has_value()) {
        updated = appendPhiIncoming(updated, scanNextState, scanFinishName);
      }
      if (quotientSummary.has_value()) {
        updated =
            appendPhiIncoming(updated, quotientNextState, quotientCommitName);
      }
      header->instructions()[index] = std::move(updated);
    } else {
      return false;
    }
  }

  auto position = std::find_if(
      function.blocks().begin(), function.blocks().end(),
      [&](const BasicBlock &block) { return block.name() == candidate.body; });
  if (position == function.blocks().end()) {
    return false;
  }
  if (quotientSummary.has_value()) {
    position = function.blocks().insert(position, std::move(quotientGuard));
    ++position;
    position = function.blocks().insert(position, std::move(quotientBody));
    ++position;
    position = function.blocks().insert(position, std::move(quotientCommit));
    ++position;
  }
  if (boundedScan.has_value()) {
    position = function.blocks().insert(position, std::move(scanGuard));
    ++position;
    position = function.blocks().insert(position, std::move(scanHeader));
    ++position;
    position = function.blocks().insert(position, std::move(scanStep));
    ++position;
    position = function.blocks().insert(position, std::move(scanFinish));
    ++position;
  }
  position = function.blocks().insert(position, std::move(guard));
  ++position;
  function.blocks().insert(position, std::move(vectorBody));
  return true;
}

} // namespace

void AArch64PureMapRecurrenceVectorizationPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_PURE_MAP_RECURRENCE_VECTOR") !=
      nullptr) {
    return;
  }
  bool transformed = false;
  for (auto &function : module.functions()) {
    for (std::size_t count = 0; count < kMaximumTransformsPerFunction;
         ++count) {
      const auto candidate = matchCandidate(function);
      if (!candidate.has_value() || !transform(function, *candidate)) {
        break;
      }
      transformed = true;
    }
  }
  if (transformed) {
    module.addPassNote(
        "vectorized pure-map recurrences and summarized proven affine "
        "constant-quotient blocks");
  }
}

} // namespace compiler::midend::pass
