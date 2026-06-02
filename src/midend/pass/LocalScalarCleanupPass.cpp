#include "../../../include/compiler/midend/pass/LocalScalarCleanupPass.h"

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

struct StoredValue {
  std::string type;
  std::string value;
};

using StoreMap = std::unordered_map<std::string, StoredValue>;

bool isSameValue(const std::string &lhs, const std::string &rhs) {
  return !lhs.empty() && lhs == rhs;
}

bool isScalarAlloca(const Instruction &instruction) {
  return instruction.opcode() == "alloca" && instruction.hasResult() &&
         instruction.operands().size() == 1 &&
         instruction.operands().front().find('[') == std::string::npos;
}

std::optional<std::string> simplifyLocalAlias(const Instruction &instruction) {
  if (!instruction.hasResult()) {
    return std::nullopt;
  }

  const auto &opcode = instruction.opcode();
  const auto &operands = instruction.operands();
  if (operands.size() != 2) {
    return std::nullopt;
  }

  const auto &lhs = operands[0];
  const auto &rhs = operands[1];
  if (opcode == "add" || opcode == "fadd") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (detail::isZero(lhs)) {
      return rhs;
    }
  }
  if (opcode == "sub" || opcode == "fsub") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (opcode == "sub" && isSameValue(lhs, rhs)) {
      return "0";
    }
  }
  if (opcode == "mul" || opcode == "fmul") {
    if (opcode == "mul" && (detail::isZero(lhs) || detail::isZero(rhs))) {
      return "0";
    }
    if (detail::isOne(rhs)) {
      return lhs;
    }
    if (detail::isOne(lhs)) {
      return rhs;
    }
  }
  if ((opcode == "div" || opcode == "sdiv" || opcode == "fdiv") &&
      detail::isOne(rhs)) {
    return lhs;
  }
  if ((opcode == "mod" || opcode == "srem") && detail::isOne(rhs)) {
    return "0";
  }
  if (opcode == "and") {
    if (detail::isZero(lhs) || detail::isZero(rhs)) {
      return "0";
    }
    if (isSameValue(lhs, rhs)) {
      return lhs;
    }
  }
  if (opcode == "or") {
    if (detail::isZero(rhs)) {
      return lhs;
    }
    if (detail::isZero(lhs)) {
      return rhs;
    }
    if (isSameValue(lhs, rhs)) {
      return lhs;
    }
  }
  if (opcode.rfind("icmp.", 0) == 0 && isSameValue(lhs, rhs)) {
    const std::string predicate = opcode.substr(5);
    if (predicate == "eq" || predicate == "sle" || predicate == "sge") {
      return detail::formatBooleanLiteral(true);
    }
    if (predicate == "ne" || predicate == "slt" || predicate == "sgt") {
      return detail::formatBooleanLiteral(false);
    }
  }

  return std::nullopt;
}

std::unordered_set<std::string>
collectNonEscapedScalarSlots(const Function &function) {
  std::unordered_set<std::string> scalarAllocas;
  std::unordered_set<std::string> escapedSlots;

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (isScalarAlloca(instruction)) {
        scalarAllocas.insert(instruction.result());
      }
    }
  }

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      if (instruction.opcode() == "load" && operands.size() == 2) {
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3) {
        if (scalarAllocas.find(operands[1]) != scalarAllocas.end()) {
          escapedSlots.insert(operands[1]);
        }
        continue;
      }

      for (const auto &operand : operands) {
        if (scalarAllocas.find(operand) != scalarAllocas.end()) {
          escapedSlots.insert(operand);
        }
      }
    }
  }

  for (const auto &escaped : escapedSlots) {
    scalarAllocas.erase(escaped);
  }
  return scalarAllocas;
}

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto &operands = terminator->operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

std::unordered_map<std::string, std::vector<std::string>>
computePredecessors(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto &block : function.blocks()) {
    predecessors.emplace(block.name(), std::vector<std::string>{});
  }
  for (const auto &block : function.blocks()) {
    for (const auto &target : branchTargets(block)) {
      predecessors[target].push_back(block.name());
    }
  }
  return predecessors;
}

std::unordered_map<std::string, std::size_t>
computeSuccessorCounts(const Function &function) {
  std::unordered_map<std::string, std::size_t> successorCounts;
  for (const auto &block : function.blocks()) {
    successorCounts[block.name()] = branchTargets(block).size();
  }
  return successorCounts;
}

StoreMap forwardBlockLocalStoreLoads(
    BasicBlock &block,
    const std::unordered_set<std::string> &nonEscapedScalarSlots,
    StoreMap availableStores) {
  std::unordered_map<std::string, std::string> replacements;
  std::vector<Instruction> rewrittenInstructions;
  rewrittenInstructions.reserve(block.instructions().size());

  for (const auto &instruction : block.instructions()) {
    auto operands =
        detail::rewriteOperands(instruction.operands(), replacements);
    Instruction rewritten{instruction.opcode(), operands, instruction.result()};

    if (rewritten.opcode() == "load" && rewritten.hasResult() &&
        operands.size() == 2) {
      const auto found = availableStores.find(operands[1]);
      if (found != availableStores.end() && found->second.type == operands[0]) {
        replacements[rewritten.result()] =
            detail::resolveAlias(found->second.value, replacements);
        continue;
      }
      replacements.erase(rewritten.result());
    } else if (auto alias = simplifyLocalAlias(rewritten); alias.has_value()) {
      replacements[rewritten.result()] =
          detail::resolveAlias(*alias, replacements);
      continue;
    } else if (rewritten.hasResult()) {
      replacements.erase(rewritten.result());
    }

    rewrittenInstructions.push_back(std::move(rewritten));
    const auto &kept = rewrittenInstructions.back();
    if (kept.opcode() == "store" && kept.operands().size() == 3) {
      if (nonEscapedScalarSlots.find(kept.operands()[2]) !=
          nonEscapedScalarSlots.end()) {
        availableStores[kept.operands()[2]] =
            StoredValue{kept.operands()[0], kept.operands()[1]};
      } else {
        availableStores.clear();
      }
    } else if (kept.opcode() == "call") {
      for (auto it = availableStores.begin(); it != availableStores.end();) {
        if (nonEscapedScalarSlots.find(it->first) ==
            nonEscapedScalarSlots.end()) {
          it = availableStores.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  block.instructions() = std::move(rewrittenInstructions);
  return availableStores;
}

void cleanupUnusedScalarSlots(Function &function) {
  std::unordered_set<std::string> scalarAllocas;
  std::unordered_set<std::string> loadedSlots;
  std::unordered_set<std::string> escapedSlots;

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (isScalarAlloca(instruction)) {
        scalarAllocas.insert(instruction.result());
      }
    }
  }

  if (scalarAllocas.empty()) {
    return;
  }

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      if (instruction.opcode() == "load" && operands.size() == 2 &&
          scalarAllocas.find(operands[1]) != scalarAllocas.end()) {
        loadedSlots.insert(operands[1]);
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3) {
        if (scalarAllocas.find(operands[1]) != scalarAllocas.end()) {
          escapedSlots.insert(operands[1]);
        }
        continue;
      }

      for (const auto &operand : operands) {
        if (scalarAllocas.find(operand) != scalarAllocas.end()) {
          escapedSlots.insert(operand);
        }
      }
    }
  }

  std::unordered_set<std::string> removableSlots;
  for (const auto &slot : scalarAllocas) {
    if (loadedSlots.find(slot) == loadedSlots.end() &&
        escapedSlots.find(slot) == escapedSlots.end()) {
      removableSlots.insert(slot);
    }
  }

  if (removableSlots.empty()) {
    return;
  }

  for (auto &block : function.blocks()) {
    std::vector<Instruction> kept;
    kept.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      if (instruction.hasResult() &&
          removableSlots.find(instruction.result()) != removableSlots.end()) {
        continue;
      }
      if (instruction.opcode() == "store" && operands.size() == 3 &&
          removableSlots.find(operands[2]) != removableSlots.end()) {
        continue;
      }
      kept.push_back(instruction);
    }
    block.instructions() = std::move(kept);
  }
}

void cleanupFunction(Function &function) {
  const auto nonEscapedScalarSlots = collectNonEscapedScalarSlots(function);
  const auto predecessors = computePredecessors(function);
  const auto successorCounts = computeSuccessorCounts(function);
  std::unordered_map<std::string, StoreMap> exitStores;
  for (auto &block : function.blocks()) {
    StoreMap incomingStores;
    const auto predIt = predecessors.find(block.name());
    if (predIt != predecessors.end() && predIt->second.size() == 1) {
      const auto &pred = predIt->second.front();
      const auto succCountIt = successorCounts.find(pred);
      const auto exitIt = exitStores.find(pred);
      if (succCountIt != successorCounts.end() && succCountIt->second == 1 &&
          exitIt != exitStores.end()) {
        incomingStores = exitIt->second;
      }
    }
    exitStores[block.name()] = forwardBlockLocalStoreLoads(
        block, nonEscapedScalarSlots, std::move(incomingStores));
  }
  cleanupUnusedScalarSlots(function);
}

} // namespace

bool LocalScalarCleanupPass::run(compiler::ir::core::Module &module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto &function : module.functions()) {
    cleanupFunction(function);
  }
  return before != detail::moduleFingerprint(module);
}

} // namespace compiler::midend::pass
