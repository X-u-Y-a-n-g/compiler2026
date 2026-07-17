#include "../../../include/compiler/midend/pass/MonotonicLoopBoundPass.h"

#include <algorithm>
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

struct Definition {
  const Instruction* instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

DefinitionMap definitions(const Function& function) {
  DefinitionMap result;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        result.emplace(instruction.result(),
                       Definition{&instruction, block.name()});
      }
    }
  }
  return result;
}

BasicBlock* findBlock(Function& function, const std::string& name) {
  for (auto& block : function.blocks()) {
    if (block.name() == name) return &block;
  }
  return nullptr;
}

const BasicBlock* findBlock(const Function& function, const std::string& name) {
  for (const auto& block : function.blocks()) {
    if (block.name() == name) return &block;
  }
  return nullptr;
}

bool isUnitIncrement(const Instruction& instruction,
                     const std::string& induction) {
  if (instruction.opcode() != "add" || !instruction.hasResult() ||
      instruction.operands().size() != 2) {
    return false;
  }
  long long literal = 0;
  return (instruction.operands()[0] == induction &&
          detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
          literal == 1) ||
         (instruction.operands()[1] == induction &&
          detail::parseIntegerLiteral(instruction.operands()[0], literal) &&
          literal == 1);
}

std::optional<std::string> thresholdForAboveExit(
    const Instruction& compare, const std::string& induction,
    bool exitOnTrue) {
  if (compare.operands().size() != 2) return std::nullopt;
  const auto& lhs = compare.operands()[0];
  const auto& rhs = compare.operands()[1];
  if (exitOnTrue && compare.opcode() == "icmp.slt" && rhs == induction) {
    return lhs;
  }
  if (exitOnTrue && compare.opcode() == "icmp.sgt" && lhs == induction) {
    return rhs;
  }
  if (!exitOnTrue && compare.opcode() == "icmp.sle" && lhs == induction) {
    return rhs;
  }
  if (!exitOnTrue && compare.opcode() == "icmp.sge" && rhs == induction) {
    return lhs;
  }
  return std::nullopt;
}

bool hasPhi(const BasicBlock& block) {
  return std::any_of(block.instructions().begin(), block.instructions().end(),
                     [](const Instruction& instruction) {
                       return instruction.opcode() == "phi";
                     });
}

std::optional<std::string> uniquePreheaderFromPhi(
    const Instruction& phi, const std::string& work,
    const DefinitionMap& defs, const std::string& induction) {
  if (phi.opcode() != "phi" || phi.operands().size() < 5 ||
      phi.operands().size() % 2 == 0) {
    return std::nullopt;
  }
  std::optional<std::string> preheader;
  bool hasWorkIncrement = false;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    const auto& value = phi.operands()[index];
    const auto& block = phi.operands()[index + 1];
    if (block == work) {
      const auto found = defs.find(value);
      if (found == defs.end() ||
          !isUnitIncrement(*found->second.instruction, induction)) {
        return std::nullopt;
      }
      hasWorkIncrement = true;
      continue;
    }
    long long initial = 0;
    if (detail::parseIntegerLiteral(value, initial) && initial == 0) {
      if (preheader.has_value()) return std::nullopt;
      preheader = block;
    }
  }
  return hasWorkIncrement ? preheader : std::nullopt;
}

bool isSafeNonNegativeOuterInduction(
    const Function& function, const std::string& value,
    const std::string& innerPreheader, const std::string& innerHeader,
    const DefinitionMap& defs) {
  const auto found = defs.find(value);
  if (found == defs.end() || found->second.instruction->opcode() != "phi") {
    return false;
  }
  if (found->second.instruction->operands().empty() ||
      found->second.instruction->operands()[0] != "int") {
    return false;
  }
  const auto* outerHeader = findBlock(function, found->second.block);
  if (outerHeader == nullptr || outerHeader->terminator() == nullptr ||
      outerHeader->terminator()->opcode() != "br" ||
      outerHeader->terminator()->operands().size() != 3) {
    return false;
  }
  const auto& trueTarget = outerHeader->terminator()->operands()[1];
  if (!((outerHeader->name() == innerPreheader &&
         trueTarget == innerHeader) ||
        trueTarget == innerPreheader)) {
    return false;
  }
  const auto condition = defs.find(outerHeader->terminator()->operands()[0]);
  if (condition == defs.end() ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2 ||
      condition->second.instruction->operands()[0] != value) {
    return false;
  }

  bool hasZero = false;
  bool hasIncrement = false;
  const auto& operands = found->second.instruction->operands();
  for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
    long long initial = 0;
    if (detail::parseIntegerLiteral(operands[index], initial) && initial == 0) {
      hasZero = true;
      continue;
    }
    const auto next = defs.find(operands[index]);
    if (next == defs.end() ||
        !isUnitIncrement(*next->second.instruction, value)) {
      return false;
    }
    hasIncrement = true;
  }
  return hasZero && hasIncrement;
}

std::string freshValue(std::unordered_set<std::string>& names,
                       const std::string& stem) {
  std::string result = stem;
  std::size_t suffix = 0;
  while (!names.insert(result).second) {
    result = stem + "." + std::to_string(++suffix);
  }
  return result;
}

bool tightenOneLoop(Function& function) {
  const auto defs = definitions(function);
  std::unordered_set<std::string> names;
  for (const auto& [name, unused] : defs) {
    (void)unused;
    names.insert(name);
  }

  for (auto& header : function.blocks()) {
    const auto* terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto condition = defs.find(terminator->operands()[0]);
    if (condition == defs.end() ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2) {
      continue;
    }
    const std::string induction =
        condition->second.instruction->operands()[0];
    const std::string upperBound =
        condition->second.instruction->operands()[1];
    const std::string guardName = terminator->operands()[1];
    const std::string exitName = terminator->operands()[2];
    auto* guard = findBlock(function, guardName);
    if (guard == nullptr || guard->instructions().size() != 2 ||
        guard->terminator() == nullptr ||
        guard->terminator()->opcode() != "br" ||
        guard->terminator()->operands().size() != 3) {
      continue;
    }
    const auto guardCondition = defs.find(guard->terminator()->operands()[0]);
    if (guardCondition == defs.end()) continue;

    for (int exitIndex : {1, 2}) {
      const int workIndex = exitIndex == 1 ? 2 : 1;
      if (guard->terminator()->operands()[exitIndex] != exitName) continue;
      const std::string workName =
          guard->terminator()->operands()[workIndex];
      auto* work = findBlock(function, workName);
      if (work == nullptr || hasPhi(*work) || work->terminator() == nullptr ||
          work->terminator()->opcode() != "br" ||
          work->terminator()->operands() !=
              std::vector<std::string>{header.name()}) {
        continue;
      }
      const auto threshold = thresholdForAboveExit(
          *guardCondition->second.instruction, induction, exitIndex == 1);
      if (!threshold.has_value()) continue;

      const Instruction* phi = nullptr;
      for (const auto& instruction : header.instructions()) {
        if (instruction.opcode() == "phi" &&
            instruction.result() == induction) {
          if (phi != nullptr) {
            phi = nullptr;
            break;
          }
          phi = &instruction;
        }
      }
      if (phi == nullptr) continue;
      const auto preheaderName =
          uniquePreheaderFromPhi(*phi, workName, defs, induction);
      if (!preheaderName.has_value() ||
          !isSafeNonNegativeOuterInduction(function, *threshold,
                                           *preheaderName, header.name(),
                                           defs)) {
        continue;
      }
      auto* preheader = findBlock(function, *preheaderName);
      if (preheader == nullptr || preheader->terminator() == nullptr) continue;

      const auto next = freshValue(names, "%loop.bound.next");
      const auto choose = freshValue(names, "%loop.bound.choose");
      const auto tight = freshValue(names, "%loop.bound.tight");
      auto insert = preheader->instructions().end() - 1;
      insert = preheader->instructions().insert(
          insert, Instruction{"add", {*threshold, "1"}, next});
      ++insert;
      insert = preheader->instructions().insert(
          insert, Instruction{"icmp.slt", {next, upperBound}, choose});
      ++insert;
      preheader->instructions().insert(
          insert, Instruction{"select", {choose, next, upperBound}, tight});

      auto compareOperands = condition->second.instruction->operands();
      compareOperands[1] = tight;
      for (auto& instruction : header.instructions()) {
        if (instruction.hasResult() ==
                condition->second.instruction->hasResult() &&
            instruction.result() == condition->second.instruction->result()) {
          instruction = Instruction{"icmp.slt", std::move(compareOperands),
                                    instruction.result()};
          break;
        }
      }
      auto branchOperands = terminator->operands();
      branchOperands[1] = workName;
      header.instructions().back() =
          Instruction{"br", std::move(branchOperands)};
      return true;
    }
  }
  return false;
}

}  // namespace

void MonotonicLoopBoundPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    while (tightenOneLoop(function)) {
    }
  }
}

}  // namespace compiler::midend::pass
