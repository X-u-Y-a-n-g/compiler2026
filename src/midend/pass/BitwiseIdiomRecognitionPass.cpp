#include "../../../include/compiler/midend/pass/BitwiseIdiomRecognitionPass.h"

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

struct LocatedInstruction {
  const Instruction* instruction = nullptr;
  std::string block;
};

struct BitwiseIdiom {
  std::string opcode;
  std::string fallbackTarget;
};

bool isIntegerParameter(const compiler::ir::core::FunctionParameter& parameter) {
  return parameter.type == "int" || parameter.type == "i32";
}

bool isBinaryWithLiteral(const Instruction& instruction, const std::string& opcode,
                         const std::string& literal) {
  return instruction.opcode() == opcode && instruction.operands().size() == 2 &&
         instruction.operands()[1] == literal;
}

std::optional<BitwiseIdiom> matchBitwiseIdiom(const Function& function) {
  if ((function.returnType() != "int" && function.returnType() != "i32") ||
      function.parameters().size() != 2 ||
      !isIntegerParameter(function.parameters()[0]) ||
      !isIntegerParameter(function.parameters()[1]) || function.blocks().size() < 5 ||
      function.blocks().size() > 7) {
    return std::nullopt;
  }

  const auto& entry = function.blocks().front();
  if (entry.instructions().size() != 1 ||
      entry.instructions().front().opcode() != "br" ||
      entry.instructions().front().operands().size() != 1) {
    return std::nullopt;
  }

  std::unordered_map<std::string, LocatedInstruction> definitions;
  std::unordered_map<std::string, const BasicBlock*> blocks;
  std::unordered_set<std::string> remainderResults;
  std::unordered_set<std::string> allowedOpcodes = {
      "phi", "br", "ret", "srem", "sdiv", "add", "mul", "sub", "icmp.eq", "icmp.ne"};
  std::size_t phiCount = 0;
  std::size_t remainderCount = 0;
  std::size_t divisionCount = 0;
  std::size_t addCount = 0;
  std::size_t multiplyCount = 0;
  std::size_t subtractCount = 0;
  std::size_t returnCount = 0;
  bool hasLengthInitialization = false;
  bool hasZeroAccumulator = false;
  bool hasUnitPower = false;

  for (const auto& block : function.blocks()) {
    blocks.emplace(block.name(), &block);
    for (const auto& instruction : block.instructions()) {
      if (allowedOpcodes.find(instruction.opcode()) == allowedOpcodes.end()) {
        return std::nullopt;
      }
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), LocatedInstruction{&instruction, block.name()});
      }
      if (instruction.opcode() == "phi") {
        ++phiCount;
        if (instruction.operands().size() < 3 || instruction.operands().size() % 2 == 0) {
          return std::nullopt;
        }
        for (std::size_t i = 1; i + 1 < instruction.operands().size(); i += 2) {
          if (instruction.operands()[i + 1] != entry.name()) {
            continue;
          }
          hasLengthInitialization |= instruction.operands()[i] == "32";
          hasZeroAccumulator |= instruction.operands()[i] == "0";
          hasUnitPower |= instruction.operands()[i] == "1";
        }
      } else if (isBinaryWithLiteral(instruction, "srem", "2")) {
        ++remainderCount;
        remainderResults.insert(instruction.result());
      } else if (isBinaryWithLiteral(instruction, "sdiv", "2")) {
        ++divisionCount;
      } else if (instruction.opcode() == "add") {
        ++addCount;
      } else if (isBinaryWithLiteral(instruction, "mul", "2")) {
        ++multiplyCount;
      } else if (isBinaryWithLiteral(instruction, "sub", "1")) {
        ++subtractCount;
      } else if (instruction.opcode() == "ret") {
        ++returnCount;
      }
    }
  }

  if (phiCount != 6 || remainderCount != 2 || divisionCount != 2 || addCount != 1 ||
      multiplyCount != 1 || subtractCount != 1 || returnCount != 1 ||
      !hasLengthInitialization || !hasZeroAccumulator || !hasUnitPower) {
    return std::nullopt;
  }

  const LocatedInstruction* xorCompare = nullptr;
  std::vector<const LocatedInstruction*> bitTests;
  for (const auto& [unusedName, located] : definitions) {
    (void)unusedName;
    const auto& instruction = *located.instruction;
    if (instruction.opcode() == "icmp.ne" && instruction.operands().size() == 2 &&
        remainderResults.find(instruction.operands()[0]) != remainderResults.end() &&
        remainderResults.find(instruction.operands()[1]) != remainderResults.end()) {
      if (xorCompare != nullptr) {
        return std::nullopt;
      }
      xorCompare = &located;
    }
    if (instruction.opcode() == "icmp.eq" && instruction.operands().size() == 2 &&
        instruction.operands()[1] == "1" &&
        remainderResults.find(instruction.operands()[0]) != remainderResults.end()) {
      bitTests.push_back(&located);
    }
  }

  if (xorCompare != nullptr) {
    if (!bitTests.empty()) {
      return std::nullopt;
    }
    const auto blockIt = blocks.find(xorCompare->block);
    if (blockIt == blocks.end()) {
      return std::nullopt;
    }
    const auto* terminator = blockIt->second->terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3 ||
        terminator->operands()[0] != xorCompare->instruction->result()) {
      return std::nullopt;
    }
    return BitwiseIdiom{"xor", entry.instructions().front().operands()[0]};
  }

  if (bitTests.size() != 2) {
    return std::nullopt;
  }
  const auto firstBlockIt = blocks.find(bitTests[0]->block);
  const auto secondBlockIt = blocks.find(bitTests[1]->block);
  if (firstBlockIt == blocks.end() || secondBlockIt == blocks.end()) {
    return std::nullopt;
  }
  const auto* firstBranch = firstBlockIt->second->terminator();
  const auto* secondBranch = secondBlockIt->second->terminator();
  if (firstBranch == nullptr || secondBranch == nullptr ||
      firstBranch->operands().size() != 3 || secondBranch->operands().size() != 3 ||
      firstBranch->operands()[0] != bitTests[0]->instruction->result() ||
      secondBranch->operands()[0] != bitTests[1]->instruction->result()) {
    return std::nullopt;
  }

  const bool firstTrueToSecond = firstBranch->operands()[1] == bitTests[1]->block;
  const bool firstFalseToSecond = firstBranch->operands()[2] == bitTests[1]->block;
  const bool secondTrueToFirst = secondBranch->operands()[1] == bitTests[0]->block;
  const bool secondFalseToFirst = secondBranch->operands()[2] == bitTests[0]->block;
  if (firstTrueToSecond || secondTrueToFirst) {
    return BitwiseIdiom{"and", entry.instructions().front().operands()[0]};
  }
  if (firstFalseToSecond || secondFalseToFirst) {
    return BitwiseIdiom{"or", entry.instructions().front().operands()[0]};
  }
  return std::nullopt;
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

void rewriteEntryPhiLabels(Function& function, const std::string& oldEntry,
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
      for (std::size_t i = 2; i < operands.size(); i += 2) {
        if (operands[i] == oldEntry) {
          operands[i] = fallback;
        }
      }
      rewritten.emplace_back("phi", std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void addNonnegativeFastPath(Function& function, const BitwiseIdiom& idiom) {
  const std::string entryName = function.blocks().front().name();
  const std::string fallbackName = freshBlockName(function, "bitwise.fallback");
  const std::string fastName = freshBlockName(function, "bitwise.fast");
  const std::string lhsNonnegative = freshValueName(function, "bitwise.lhs.nonnegative");
  const std::string rhsNonnegative = freshValueName(function, "bitwise.rhs.nonnegative");
  const std::string bothNonnegative = freshValueName(function, "bitwise.both.nonnegative");
  const std::string result = freshValueName(function, "bitwise.result");

  rewriteEntryPhiLabels(function, entryName, fallbackName);

  auto& entryInstructions = function.blocks().front().instructions();
  entryInstructions.clear();
  entryInstructions.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[0].name, "0"}, lhsNonnegative);
  entryInstructions.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[1].name, "0"}, rhsNonnegative);
  entryInstructions.emplace_back(
      "and", std::vector<std::string>{lhsNonnegative, rhsNonnegative}, bothNonnegative);
  entryInstructions.emplace_back(
      "br", std::vector<std::string>{bothNonnegative, fastName, fallbackName});

  BasicBlock fallback{fallbackName};
  fallback.addInstruction(Instruction{"br", {idiom.fallbackTarget}});
  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{idiom.opcode,
                                  {function.parameters()[0].name, function.parameters()[1].name},
                                  result});
  fast.addInstruction(Instruction{"ret", {result}});
  function.blocks().insert(function.blocks().begin() + 1, std::move(fast));
  function.blocks().insert(function.blocks().begin() + 2, std::move(fallback));
}

}  // namespace

void BitwiseIdiomRecognitionPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    const auto idiom = matchBitwiseIdiom(function);
    if (idiom.has_value()) {
      addNonnegativeFastPath(function, *idiom);
    }
  }
}

}  // namespace compiler::midend::pass
