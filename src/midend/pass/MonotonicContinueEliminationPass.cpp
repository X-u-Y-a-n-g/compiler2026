#include "../../../include/compiler/midend/pass/MonotonicContinueEliminationPass.h"

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

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> predecessors;
};

struct PhiIncoming {
  std::string value;
  std::string block;
};

struct Candidate {
  std::string guard;
  std::string skip;
  std::string exit;
};

using DefinitionMap = std::unordered_map<std::string, const Instruction*>;

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (const auto& block : function.blocks()) {
    const auto* terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto addTarget = [&](const std::string& target) {
      const auto found = cfg.index.find(target);
      if (found != cfg.index.end()) {
        cfg.predecessors[found->second].push_back(block.name());
      }
    };
    const auto& operands = terminator->operands();
    if (operands.size() == 1) {
      addTarget(operands[0]);
    } else if (operands.size() == 3) {
      addTarget(operands[1]);
      if (operands[2] != operands[1]) {
        addTarget(operands[2]);
      }
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
      }
    }
  }
  return definitions;
}

const BasicBlock* findBlock(const Function& function, const CFG& cfg,
                            const std::string& name) {
  const auto found = cfg.index.find(name);
  return found == cfg.index.end() ? nullptr : &function.blocks()[found->second];
}

BasicBlock* findBlock(Function& function, const std::string& name) {
  for (auto& block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

std::optional<std::vector<PhiIncoming>> parsePhi(
    const Instruction& instruction) {
  if (instruction.opcode() != "phi" || !instruction.hasResult() ||
      instruction.operands().size() < 3 ||
      instruction.operands().size() % 2 == 0) {
    return std::nullopt;
  }
  std::vector<PhiIncoming> incoming;
  for (std::size_t index = 1; index + 1 < instruction.operands().size();
       index += 2) {
    incoming.push_back(
        PhiIncoming{instruction.operands()[index],
                    instruction.operands()[index + 1]});
  }
  return incoming;
}

bool hasOnlyPredecessor(const CFG& cfg, const std::string& block,
                        const std::string& expected) {
  const auto found = cfg.index.find(block);
  return found != cfg.index.end() &&
         cfg.predecessors[found->second] ==
             std::vector<std::string>{expected};
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

std::optional<std::string> unitIncrementResult(
    const BasicBlock& block, const std::string& induction,
    bool requireOnlyIncrement) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 1) {
    return std::nullopt;
  }
  std::optional<std::string> result;
  std::size_t nonTerminatorCount = 0;
  for (const auto& instruction : block.instructions()) {
    if (instruction.isTerminator()) {
      continue;
    }
    ++nonTerminatorCount;
    if (isUnitIncrement(instruction, induction)) {
      if (result.has_value()) {
        return std::nullopt;
      }
      result = instruction.result();
    }
  }
  if (!result.has_value() ||
      (requireOnlyIncrement && nonTerminatorCount != 1)) {
    return std::nullopt;
  }
  return result;
}

bool hasObservableWork(const BasicBlock& block) {
  for (const auto& instruction : block.instructions()) {
    if (instruction.opcode() == "store" || instruction.opcode() == "call") {
      return true;
    }
  }
  return false;
}

bool isDefinedInBlocks(const Function& function, const std::string& value,
                       const std::unordered_set<std::string>& blocks) {
  if (value.empty() || value.front() != '%') {
    return false;
  }
  for (const auto& block : function.blocks()) {
    if (blocks.find(block.name()) == blocks.end()) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && instruction.result() == value) {
        return true;
      }
    }
  }
  return false;
}

bool usesValueOutsideBlocks(const Function& function, const std::string& value,
                            const std::unordered_set<std::string>& blocks) {
  for (const auto& block : function.blocks()) {
    if (blocks.find(block.name()) != blocks.end()) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      if (std::find(operands.begin(), operands.end(), value) !=
          operands.end()) {
        return true;
      }
    }
  }
  return false;
}

std::optional<std::string> thresholdWhenSkipMeansAbove(
    const Instruction& condition, const std::string& induction,
    bool skipOnTrue) {
  if (condition.operands().size() != 2) {
    return std::nullopt;
  }
  const auto& lhs = condition.operands()[0];
  const auto& rhs = condition.operands()[1];
  if (skipOnTrue && condition.opcode() == "icmp.slt" && rhs == induction) {
    return lhs;
  }
  if (skipOnTrue && condition.opcode() == "icmp.sgt" && lhs == induction) {
    return rhs;
  }
  if (!skipOnTrue && condition.opcode() == "icmp.sle" && lhs == induction) {
    return rhs;
  }
  if (!skipOnTrue && condition.opcode() == "icmp.sge" && rhs == induction) {
    return lhs;
  }
  return std::nullopt;
}

std::vector<Candidate> findCandidates(const Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  std::vector<Candidate> candidates;

  for (const auto& header : function.blocks()) {
    const auto* headerTerminator = header.terminator();
    if (headerTerminator == nullptr || headerTerminator->opcode() != "br" ||
        headerTerminator->operands().size() != 3) {
      continue;
    }
    const auto condition = definitions.find(headerTerminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second->opcode() != "icmp.slt" ||
        condition->second->operands().size() != 2) {
      continue;
    }
    const std::string induction = condition->second->operands()[0];
    const std::string upperBound = condition->second->operands()[1];

    const Instruction* inductionPhi = nullptr;
    std::size_t phiCount = 0;
    for (const auto& instruction : header.instructions()) {
      if (instruction.opcode() != "phi") {
        continue;
      }
      ++phiCount;
      if (instruction.result() == induction) {
        inductionPhi = &instruction;
      }
    }
    if (phiCount != 1 || inductionPhi == nullptr ||
        inductionPhi->operands().empty() ||
        inductionPhi->operands()[0] != "int") {
      continue;
    }
    const auto incoming = parsePhi(*inductionPhi);
    if (!incoming.has_value() || incoming->size() != 3) {
      continue;
    }

    const std::string guardName = headerTerminator->operands()[1];
    const std::string exitName = headerTerminator->operands()[2];
    if (guardName == exitName || guardName == header.name() ||
        exitName == header.name()) {
      continue;
    }
    const auto* guard = findBlock(function, cfg, guardName);
    const auto* exit = findBlock(function, cfg, exitName);
    if (guard == nullptr || exit == nullptr ||
        !hasOnlyPredecessor(cfg, guardName, header.name()) ||
        (!exit->instructions().empty() &&
         exit->instructions().front().opcode() == "phi") ||
        guard->instructions().size() != 2) {
      continue;
    }
    const auto* guardTerminator = guard->terminator();
    if (guardTerminator == nullptr || guardTerminator->opcode() != "br" ||
        guardTerminator->operands().size() != 3) {
      continue;
    }
    const auto guardCondition =
        definitions.find(guardTerminator->operands()[0]);
    if (guardCondition == definitions.end() ||
        guard->instructions().front().result() !=
            guardCondition->second->result()) {
      continue;
    }

    for (int skipIndex : {1, 2}) {
      const int workIndex = skipIndex == 1 ? 2 : 1;
      const std::string skipName = guardTerminator->operands()[skipIndex];
      const std::string workName = guardTerminator->operands()[workIndex];
      if (skipName == workName || skipName == header.name() ||
          workName == header.name() || skipName == exitName ||
          workName == exitName) {
        continue;
      }
      const auto* skip = findBlock(function, cfg, skipName);
      const auto* work = findBlock(function, cfg, workName);
      if (skip == nullptr || work == nullptr ||
          !hasOnlyPredecessor(cfg, skipName, guardName) ||
          !hasOnlyPredecessor(cfg, workName, guardName) ||
          skip->terminator() == nullptr || work->terminator() == nullptr ||
          skip->terminator()->operands() !=
              std::vector<std::string>{header.name()} ||
          work->terminator()->operands() !=
              std::vector<std::string>{header.name()} ||
          !hasObservableWork(*work)) {
        continue;
      }
      const auto skipNext = unitIncrementResult(*skip, induction, true);
      const auto workNext = unitIncrementResult(*work, induction, false);
      if (!skipNext.has_value() || !workNext.has_value()) {
        continue;
      }
      const auto threshold = thresholdWhenSkipMeansAbove(
          *guardCondition->second, induction, skipIndex == 1);
      if (!threshold.has_value()) {
        continue;
      }

      std::string preheader;
      bool incomingMatch = true;
      for (const auto& edge : *incoming) {
        if (edge.block == skipName) {
          incomingMatch &= edge.value == *skipNext;
        } else if (edge.block == workName) {
          incomingMatch &= edge.value == *workNext;
        } else if (preheader.empty()) {
          long long initial = 0;
          preheader = edge.block;
          incomingMatch &=
              detail::parseIntegerLiteral(edge.value, initial) && initial == 0;
        } else {
          incomingMatch = false;
        }
      }
      const auto headerIndex = cfg.index.find(header.name());
      if (!incomingMatch || preheader.empty() ||
          preheader == header.name() || preheader == guardName ||
          preheader == skipName || preheader == workName ||
          headerIndex == cfg.index.end() ||
          cfg.predecessors[headerIndex->second].size() != 3 ||
          std::find(cfg.predecessors[headerIndex->second].begin(),
                    cfg.predecessors[headerIndex->second].end(), preheader) ==
              cfg.predecessors[headerIndex->second].end()) {
        continue;
      }

      const std::unordered_set<std::string> loopBlocks{
          header.name(), guardName, skipName, workName};
      if (isDefinedInBlocks(function, upperBound, loopBlocks) ||
          isDefinedInBlocks(function, *threshold, loopBlocks) ||
          usesValueOutsideBlocks(function, induction, loopBlocks) ||
          usesValueOutsideBlocks(function, *skipNext, loopBlocks) ||
          usesValueOutsideBlocks(function, *workNext, loopBlocks)) {
        continue;
      }

      candidates.push_back(Candidate{guardName, skipName, exitName});
      break;
    }
  }
  return candidates;
}

void eliminateMonotonicContinues(Function& function) {
  for (const auto& candidate : findCandidates(function)) {
    auto* guard = findBlock(function, candidate.guard);
    if (guard == nullptr || guard->instructions().empty()) {
      continue;
    }
    const auto& terminator = guard->instructions().back();
    if (terminator.opcode() != "br" || terminator.operands().size() != 3) {
      continue;
    }
    auto operands = terminator.operands();
    if (operands[1] == candidate.skip) {
      operands[1] = candidate.exit;
    } else if (operands[2] == candidate.skip) {
      operands[2] = candidate.exit;
    } else {
      continue;
    }
    guard->instructions().back() = Instruction{"br", std::move(operands)};
  }
}

}  // namespace

void MonotonicContinueEliminationPass::run(
    compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    eliminateMonotonicContinues(function);
  }
}

}  // namespace compiler::midend::pass
