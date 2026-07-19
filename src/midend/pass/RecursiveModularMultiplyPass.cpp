#include "../../../include/compiler/midend/pass/RecursiveModularMultiplyPass.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

using DefinitionMap = std::unordered_map<std::string, const Instruction*>;

DefinitionMap definitions(const Function& function) {
  DefinitionMap result;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) result.emplace(instruction.result(), &instruction);
    }
  }
  return result;
}

const Instruction* definition(const DefinitionMap& defs,
                              const std::string& value) {
  const auto found = defs.find(value);
  return found == defs.end() ? nullptr : found->second;
}

bool binaryUses(const Instruction& instruction, const std::string& lhs,
                const std::string& rhs) {
  return instruction.operands().size() == 2 &&
         ((instruction.operands()[0] == lhs && instruction.operands()[1] == rhs) ||
          (instruction.operands()[0] == rhs && instruction.operands()[1] == lhs));
}

bool isLiteral(const std::string& operand, long long expected) {
  long long value = 0;
  return detail::parseIntegerLiteral(operand, value) && value == expected;
}

bool loadsFrom(const DefinitionMap& defs, const std::string& value,
               const std::string& address) {
  const auto* load = definition(defs, value);
  return load != nullptr && load->opcode() == "load" &&
         load->operands().size() == 2 && load->operands()[1] == address;
}

struct Candidate {
  std::string multiplicand;
  std::string multiplier;
  long long modulus = 0;
};

std::optional<Candidate> findCandidate(const Function& function) {
  if (function.returnType() != "int" || function.parameters().size() != 2 ||
      function.parameters()[0].type != "int" ||
      function.parameters()[1].type != "int" || function.blocks().empty()) {
    return std::nullopt;
  }
  const std::string a = function.parameters()[0].name;
  const std::string b = function.parameters()[1].name;
  const auto defs = definitions(function);

  const Instruction* selfCall = nullptr;
  std::size_t selfCallCount = 0;
  std::string stateAddress;
  std::vector<const Instruction*> modulusRemainders;
  const Instruction* oddRemainder = nullptr;
  const Instruction* oddCompare = nullptr;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      const auto& opcode = instruction.opcode();
      if (opcode == "call") {
        if (instruction.operands().empty() ||
            instruction.operands()[0] != "@" + function.name()) {
          return std::nullopt;
        }
        ++selfCallCount;
        selfCall = &instruction;
      } else if (opcode == "srem" && instruction.operands().size() == 2) {
        if (instruction.operands()[0] == b &&
            isLiteral(instruction.operands()[1], 2)) {
          if (oddRemainder != nullptr) return std::nullopt;
          oddRemainder = &instruction;
        } else {
          modulusRemainders.push_back(&instruction);
        }
      }
      static const std::unordered_set<std::string> allowed{
          "alloca", "load", "store", "add", "sdiv", "srem", "icmp.eq",
          "br", "ret", "call"};
      if (allowed.find(opcode) == allowed.end()) return std::nullopt;
    }
  }
  if (selfCallCount != 1 || selfCall == nullptr || !selfCall->hasResult() ||
      selfCall->operands().size() != 3 || selfCall->operands()[1] != a ||
      modulusRemainders.size() != 3 || oddRemainder == nullptr) {
    return std::nullopt;
  }
  const auto* half = definition(defs, selfCall->operands()[2]);
  if (half == nullptr || half->opcode() != "sdiv" ||
      half->operands().size() != 2 || half->operands()[0] != b ||
      !isLiteral(half->operands()[1], 2)) {
    return std::nullopt;
  }

  long long modulus = 0;
  bool sawBase = false;
  const Instruction* baseRemainder = nullptr;
  const Instruction* doubled = nullptr;
  const Instruction* doubledRemainder = nullptr;
  const Instruction* finalRemainder = nullptr;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "store" && instruction.operands().size() == 3 &&
          instruction.operands()[1] == selfCall->result()) {
        if (!stateAddress.empty() && stateAddress != instruction.operands()[2]) {
          return std::nullopt;
        }
        stateAddress = instruction.operands()[2];
      }
    }
  }
  if (stateAddress.empty()) return std::nullopt;
  for (const auto* remainder : modulusRemainders) {
    long long current = 0;
    if (!detail::parseIntegerLiteral(remainder->operands()[1], current) ||
        current <= 1 || current > 1073741824LL) {
      return std::nullopt;
    }
    if (modulus == 0) modulus = current;
    if (current != modulus) return std::nullopt;
    if (remainder->operands()[0] == a) {
      sawBase = true;
      baseRemainder = remainder;
      continue;
    }
    const auto* source = definition(defs, remainder->operands()[0]);
    if (source == nullptr || source->opcode() != "add") return std::nullopt;
    const bool doublesCall =
        binaryUses(*source, selfCall->result(), selfCall->result()) ||
        (source->operands().size() == 2 &&
         loadsFrom(defs, source->operands()[0], stateAddress) &&
         loadsFrom(defs, source->operands()[1], stateAddress));
    if (doublesCall) {
      doubled = source;
      doubledRemainder = remainder;
    } else if (source->operands().size() == 2 &&
               (source->operands()[0] == a || source->operands()[1] == a)) {
      finalRemainder = remainder;
    } else {
      return std::nullopt;
    }
  }
  if (!sawBase || doubled == nullptr || doubledRemainder == nullptr ||
      finalRemainder == nullptr) {
    return std::nullopt;
  }

  bool sawDoubledStore = false;
  bool sawOddBranch = false;
  const Instruction* zeroCompare = nullptr;
  const Instruction* oneCompare = nullptr;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "store") {
        if (instruction.operands().size() != 3 ||
            instruction.operands()[2] != stateAddress) {
          return std::nullopt;
        }
        const auto& value = instruction.operands()[1];
        if (value != selfCall->result() &&
            value != doubledRemainder->result()) {
          return std::nullopt;
        }
        sawDoubledStore |= value == doubledRemainder->result();
      }
      if (instruction.opcode() == "icmp.eq") {
        if (binaryUses(instruction, oddRemainder->result(), "1")) {
          oddCompare = &instruction;
        } else if (binaryUses(instruction, b, "0")) {
          zeroCompare = &instruction;
        } else if (binaryUses(instruction, b, "1")) {
          oneCompare = &instruction;
        }
      }
    }
  }
  if (stateAddress.empty() || oddCompare == nullptr || zeroCompare == nullptr ||
      oneCompare == nullptr || !sawDoubledStore) {
    return std::nullopt;
  }
  bool sawZeroBranch = false;
  bool sawOneBranch = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "br" || instruction.operands().empty()) {
        continue;
      }
      sawOddBranch |= instruction.operands()[0] == oddCompare->result();
      sawZeroBranch |= instruction.operands()[0] == zeroCompare->result();
      sawOneBranch |= instruction.operands()[0] == oneCompare->result();
    }
  }
  if (!sawOddBranch || !sawZeroBranch || !sawOneBranch) return std::nullopt;
  const auto* finalAdd = definition(defs, finalRemainder->operands()[0]);
  const std::string finalOther = finalAdd->operands()[0] == a
                                     ? finalAdd->operands()[1]
                                     : finalAdd->operands()[0];
  if (!loadsFrom(defs, finalOther, stateAddress)) return std::nullopt;

  bool sawZeroReturn = false;
  bool sawBaseReturn = false;
  bool sawFinalReturn = false;
  bool sawStateReturn = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "ret" || instruction.operands().size() != 1) {
        continue;
      }
      const auto& value = instruction.operands()[0];
      const bool returnsState = loadsFrom(defs, value, stateAddress);
      if (value != "0" &&
          (baseRemainder == nullptr || value != baseRemainder->result()) &&
          value != finalRemainder->result() && !returnsState) {
        return std::nullopt;
      }
      sawZeroReturn |= value == "0";
      sawBaseReturn |= baseRemainder != nullptr &&
                       value == baseRemainder->result();
      sawFinalReturn |= value == finalRemainder->result();
      sawStateReturn |= returnsState;
    }
  }
  if (!sawZeroReturn || !sawBaseReturn || !sawFinalReturn || !sawStateReturn) {
    return std::nullopt;
  }
  return Candidate{a, b, modulus};
}

std::string fresh(std::unordered_set<std::string>& names,
                  const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string value = stem + "." + std::to_string(suffix);
    if (names.insert(value).second) return value;
  }
}

void transform(Function& function, const Candidate& candidate) {
  std::unordered_set<std::string> values;
  std::unordered_set<std::string> blocks;
  for (const auto& parameter : function.parameters()) values.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    blocks.insert(block.name());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) values.insert(instruction.result());
    }
  }
  const auto oldEntry = function.blocks().front().name();
  const auto guardName = fresh(blocks, "modmul.guard");
  const auto fastName = fresh(blocks, "modmul.fast");
  const auto aNonNegative = fresh(values, "%modmul.a.nonnegative");
  const auto aInRange = fresh(values, "%modmul.a.inrange");
  const auto bNonNegative = fresh(values, "%modmul.b.nonnegative");
  const auto bInRange = fresh(values, "%modmul.b.inrange");
  const auto aValid = fresh(values, "%modmul.a.valid");
  const auto bValid = fresh(values, "%modmul.b.valid");
  const auto valid = fresh(values, "%modmul.valid");
  const auto a64 = fresh(values, "%modmul.a64");
  const auto b64 = fresh(values, "%modmul.b64");
  const auto product = fresh(values, "%modmul.product");
  const auto remainder = fresh(values, "%modmul.remainder");
  const auto result = fresh(values, "%modmul.result");
  const auto modulus = std::to_string(candidate.modulus);

  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"icmp.sge", {candidate.multiplicand, "0"}, aNonNegative});
  guard.addInstruction(Instruction{"icmp.slt", {candidate.multiplicand, modulus},
                                   aInRange});
  guard.addInstruction(
      Instruction{"icmp.sge", {candidate.multiplier, "0"}, bNonNegative});
  guard.addInstruction(
      Instruction{"icmp.slt", {candidate.multiplier, modulus}, bInRange});
  guard.addInstruction(Instruction{"and", {aNonNegative, aInRange}, aValid});
  guard.addInstruction(Instruction{"and", {bNonNegative, bInRange}, bValid});
  guard.addInstruction(Instruction{"and", {aValid, bValid}, valid});
  guard.addInstruction(Instruction{"br", {valid, fastName, oldEntry}});

  BasicBlock fast{fastName};
  fast.addInstruction(
      Instruction{"sext", {candidate.multiplicand, "long"}, a64});
  fast.addInstruction(Instruction{"sext", {candidate.multiplier, "long"}, b64});
  fast.addInstruction(Instruction{"mul", {a64, b64}, product});
  fast.addInstruction(Instruction{"srem", {product, modulus}, remainder});
  fast.addInstruction(Instruction{"trunc", {remainder, "int"}, result});
  fast.addInstruction(Instruction{"ret", {result}});

  function.blocks().push_front(std::move(fast));
  function.blocks().push_front(std::move(guard));
}

}  // namespace

void RecursiveModularMultiplyPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    if (const auto candidate = findCandidate(function)) {
      transform(function, *candidate);
    }
  }
}

}  // namespace compiler::midend::pass
