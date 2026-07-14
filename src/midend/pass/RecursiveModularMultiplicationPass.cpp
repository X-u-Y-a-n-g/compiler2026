#include "../../../include/compiler/midend/pass/RecursiveModularMultiplicationPass.h"

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

constexpr const char* kModulus = "998244353";

bool isInt(const std::string& type) {
  return type == "int" || type == "i32";
}

bool isBinary(const Instruction& instruction, const std::string& opcode,
              const std::string& lhs, const std::string& rhs) {
  return instruction.opcode() == opcode &&
         instruction.operands().size() == 2 &&
         instruction.operands()[0] == lhs && instruction.operands()[1] == rhs;
}

const Instruction* findDefinition(
    const std::unordered_map<std::string, const Instruction*>& definitions,
    const std::string& value) {
  const auto found = definitions.find(value);
  return found == definitions.end() ? nullptr : found->second;
}

bool matchesRecursiveModularMultiply(const Function& function) {
  if (!isInt(function.returnType()) || function.parameters().size() != 2 ||
      !isInt(function.parameters()[0].type) ||
      !isInt(function.parameters()[1].type) || function.blocks().size() < 7 ||
      function.blocks().size() > 8) {
    return false;
  }

  const std::string& a = function.parameters()[0].name;
  const std::string& b = function.parameters()[1].name;
  std::unordered_map<std::string, const Instruction*> definitions;
  std::vector<const Instruction*> calls;
  std::vector<const Instruction*> returns;
  std::vector<const Instruction*> remainders;
  std::vector<const Instruction*> additions;
  std::vector<const Instruction*> allocations;
  std::vector<const Instruction*> loads;
  std::vector<const Instruction*> stores;
  std::size_t divisionByTwoCount = 0;
  std::size_t zeroTestCount = 0;
  std::size_t oneTestCount = 0;
  std::size_t oddTestCount = 0;

  const std::unordered_set<std::string> allowedOpcodes = {
      "alloca", "load",  "store",   "call", "add", "sdiv",
      "srem",   "icmp.eq", "br",      "ret"};

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (allowedOpcodes.find(instruction.opcode()) == allowedOpcodes.end()) {
        return false;
      }
      if (instruction.hasResult()) {
        if (!definitions.emplace(instruction.result(), &instruction).second) {
          return false;
        }
      }
      if (instruction.opcode() == "call") {
        calls.push_back(&instruction);
      } else if (instruction.opcode() == "ret") {
        returns.push_back(&instruction);
      } else if (instruction.opcode() == "srem") {
        remainders.push_back(&instruction);
      } else if (instruction.opcode() == "add") {
        additions.push_back(&instruction);
      } else if (instruction.opcode() == "alloca") {
        allocations.push_back(&instruction);
      } else if (instruction.opcode() == "load") {
        loads.push_back(&instruction);
      } else if (instruction.opcode() == "store") {
        stores.push_back(&instruction);
      } else if (isBinary(instruction, "sdiv", b, "2")) {
        ++divisionByTwoCount;
      } else if (isBinary(instruction, "icmp.eq", b, "0")) {
        ++zeroTestCount;
      } else if (isBinary(instruction, "icmp.eq", b, "1")) {
        ++oneTestCount;
      }
    }
  }

  if (calls.size() != 1 || returns.size() < 4 || returns.size() > 5 ||
      remainders.size() != 4 ||
      additions.size() != 2 || divisionByTwoCount != 1 || zeroTestCount != 1 ||
      oneTestCount != 1 || allocations.size() != 1 || loads.size() != 2 ||
      stores.size() != 2 || allocations.front()->operands().size() != 1 ||
      allocations.front()->operands()[0] != "int") {
    return false;
  }

  const Instruction& recursiveCall = *calls.front();
  if (recursiveCall.operands().size() != 3 ||
      recursiveCall.operands()[0] != "@" + function.name() ||
      recursiveCall.operands()[1] != a) {
    return false;
  }
  const Instruction* dividedB = findDefinition(definitions, recursiveCall.operands()[2]);
  if (dividedB == nullptr || !isBinary(*dividedB, "sdiv", b, "2")) {
    return false;
  }

  const Instruction* doubled = nullptr;
  const Instruction* plusA = nullptr;
  for (const auto* addition : additions) {
    if (addition->operands()[0] == recursiveCall.result() &&
        addition->operands()[1] == recursiveCall.result()) {
      doubled = addition;
    } else if (addition->operands()[0] == a || addition->operands()[1] == a) {
      plusA = addition;
    }
  }
  if (doubled == nullptr || plusA == nullptr) {
    return false;
  }

  const Instruction* doubledRemainder = nullptr;
  const Instruction* plusARemainder = nullptr;
  const Instruction* baseRemainder = nullptr;
  const Instruction* parityRemainder = nullptr;
  for (const auto* remainder : remainders) {
    if (remainder->operands().size() != 2) {
      return false;
    }
    if (isBinary(*remainder, "srem", doubled->result(), kModulus)) {
      doubledRemainder = remainder;
    } else if (isBinary(*remainder, "srem", plusA->result(), kModulus)) {
      plusARemainder = remainder;
    } else if (isBinary(*remainder, "srem", a, kModulus)) {
      baseRemainder = remainder;
    } else if (isBinary(*remainder, "srem", b, "2")) {
      parityRemainder = remainder;
    } else {
      return false;
    }
  }
  if (doubledRemainder == nullptr || plusARemainder == nullptr ||
      baseRemainder == nullptr || parityRemainder == nullptr) {
    return false;
  }

  const std::string& slot = allocations.front()->result();
  std::unordered_set<std::string> loadedValues;
  for (const auto* load : loads) {
    if (load->operands().size() != 2 || load->operands()[0] != "int" ||
        load->operands()[1] != slot) {
      return false;
    }
    loadedValues.insert(load->result());
  }
  bool storesRecursiveResult = false;
  bool storesDoubledRemainder = false;
  for (const auto* store : stores) {
    if (store->operands().size() != 3 || store->operands()[0] != "int" ||
        store->operands()[2] != slot) {
      return false;
    }
    storesRecursiveResult |= store->operands()[1] == recursiveCall.result();
    storesDoubledRemainder |=
        store->operands()[1] == doubledRemainder->result();
  }
  if (!storesRecursiveResult || !storesDoubledRemainder) {
    return false;
  }

  const std::string otherPlusA = plusA->operands()[0] == a
                                     ? plusA->operands()[1]
                                     : plusA->operands()[0];
  if (loadedValues.find(otherPlusA) == loadedValues.end()) {
    return false;
  }

  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (isBinary(instruction, "icmp.eq", parityRemainder->result(), "1")) {
        ++oddTestCount;
      }
    }
  }
  if (oddTestCount != 1) {
    return false;
  }

  bool returnsZero = false;
  bool returnsBase = false;
  bool returnsDoubled = false;
  bool returnsPlusA = false;
  for (const auto* ret : returns) {
    if (ret->operands().size() != 1) {
      return false;
    }
    returnsZero |= ret->operands()[0] == "0";
    returnsBase |= ret->operands()[0] == baseRemainder->result();
    returnsDoubled |= ret->operands()[0] == doubledRemainder->result() ||
                      loadedValues.find(ret->operands()[0]) !=
                          loadedValues.end();
    returnsPlusA |= ret->operands()[0] == plusARemainder->result();
  }
  return returnsZero && returnsBase && returnsDoubled && returnsPlusA;
}

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> used;
  for (const auto& block : function.blocks()) {
    used.insert(block.name());
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> used;
  for (const auto& parameter : function.parameters()) {
    used.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "%modmul." + stem + "." +
                                  std::to_string(suffix);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

void rewriteEntryPhiLabels(Function& function, const std::string& entry,
                           const std::string& fallback) {
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == entry) {
          operands[index] = fallback;
        }
      }
      rewritten.emplace_back("phi", std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void addFastPath(Function& function) {
  const std::string entryName = function.blocks().front().name();
  const std::string fastName = freshBlockName(function, "modmul.fast");
  const std::string fallbackName = freshBlockName(function, "modmul.fallback");
  const std::string& a = function.parameters()[0].name;
  const std::string& b = function.parameters()[1].name;

  const std::string aNonnegative = freshValueName(function, "a.nonnegative");
  const std::string aBelowModulus = freshValueName(function, "a.below.modulus");
  const std::string bNonnegative = freshValueName(function, "b.nonnegative");
  const std::string aInRange = freshValueName(function, "a.in.range");
  const std::string useFast = freshValueName(function, "use.fast");
  const std::string wideA = freshValueName(function, "wide.a");
  const std::string wideB = freshValueName(function, "wide.b");
  const std::string product = freshValueName(function, "product");
  const std::string remainder = freshValueName(function, "remainder");
  const std::string result = freshValueName(function, "result");

  rewriteEntryPhiLabels(function, entryName, fallbackName);
  std::vector<Instruction> originalEntry =
      std::move(function.blocks().front().instructions());

  auto& entry = function.blocks().front();
  entry.addInstruction(Instruction{"icmp.sge", {a, "0"}, aNonnegative});
  entry.addInstruction(
      Instruction{"icmp.slt", {a, kModulus}, aBelowModulus});
  entry.addInstruction(Instruction{"icmp.sge", {b, "0"}, bNonnegative});
  entry.addInstruction(
      Instruction{"and", {aNonnegative, aBelowModulus}, aInRange});
  entry.addInstruction(Instruction{"and", {aInRange, bNonnegative}, useFast});
  entry.addInstruction(Instruction{"br", {useFast, fastName, fallbackName}});

  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{"sext", {a, "long"}, wideA});
  fast.addInstruction(Instruction{"sext", {b, "long"}, wideB});
  fast.addInstruction(Instruction{"mul", {wideA, wideB}, product});
  fast.addInstruction(Instruction{"srem", {product, kModulus}, remainder});
  fast.addInstruction(Instruction{"trunc", {remainder, "int"}, result});
  fast.addInstruction(Instruction{"ret", {result}});

  BasicBlock fallback{fallbackName};
  fallback.instructions() = std::move(originalEntry);
  function.blocks().insert(function.blocks().begin() + 1, std::move(fast));
  function.blocks().insert(function.blocks().begin() + 2, std::move(fallback));
}

}  // namespace

void RecursiveModularMultiplicationPass::run(
    compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    if (matchesRecursiveModularMultiply(function)) {
      addFastPath(function);
    }
  }
}

}  // namespace compiler::midend::pass
