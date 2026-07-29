#include "../../../include/compiler/midend/pass/DeadPrefixIterationEliminationPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;
using compiler::midend::analysis::FunctionEffectAnalysis;

struct Location {
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Definition {
  const Instruction *instruction = nullptr;
  Location location;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<std::unordered_set<std::size_t>> dominators;
};

struct CountdownLoop {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::size_t latch = 0;
  std::size_t exit = 0;
  std::string induction;
  std::string initial;
  std::string backedge;
  std::unordered_set<std::size_t> blocks;
};

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
  if (terminator->operands().size() == 1)
    return {terminator->operands()[0]};
  if (terminator->operands().size() == 3)
    return {terminator->operands()[1], terminator->operands()[2]};
  return {};
}

CFG buildCFG(const Function &function) {
  CFG cfg;
  const auto count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.dominators.resize(count);
  for (std::size_t block = 0; block < count; ++block)
    cfg.index.emplace(function.blocks()[block].name(), block);
  for (std::size_t block = 0; block < count; ++block) {
    for (const auto &target : branchTargets(function.blocks()[block])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end())
        continue;
      cfg.successors[block].push_back(found->second);
      cfg.predecessors[found->second].push_back(block);
    }
  }
  if (count == 0)
    return cfg;
  cfg.dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    for (std::size_t candidate = 0; candidate < count; ++candidate)
      cfg.dominators[block].insert(candidate);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      std::unordered_set<std::size_t> next;
      if (!cfg.predecessors[block].empty()) {
        next = cfg.dominators[cfg.predecessors[block].front()];
        for (std::size_t predecessor = 1;
             predecessor < cfg.predecessors[block].size(); ++predecessor) {
          const auto &other =
              cfg.dominators[cfg.predecessors[block][predecessor]];
          for (auto iterator = next.begin(); iterator != next.end();) {
            if (other.find(*iterator) == other.end())
              iterator = next.erase(iterator);
            else
              ++iterator;
          }
        }
      }
      next.insert(block);
      if (next != cfg.dominators[block]) {
        cfg.dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return cfg;
}

bool dominates(const CFG &cfg, std::size_t dominator, std::size_t block) {
  return block < cfg.dominators.size() &&
         cfg.dominators[block].find(dominator) != cfg.dominators[block].end();
}

DefinitionMap buildDefinitions(const Function &function) {
  DefinitionMap definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto &instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].hasResult())
        definitions.emplace(instructions[index].result(),
                            Definition{&instructions[index], {block, index}});
    }
  }
  return definitions;
}

bool isInteger(const std::string &value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

std::optional<std::pair<std::string, std::string>>
phiIncoming(const Instruction &phi, const Function &function, const CFG &cfg,
            std::size_t header) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5)
    return std::nullopt;
  std::string initial;
  std::string backedge;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    const auto predecessor = cfg.index.find(phi.operands()[index + 1]);
    if (predecessor == cfg.index.end())
      return std::nullopt;
    if (dominates(cfg, header, predecessor->second)) {
      if (!backedge.empty())
        return std::nullopt;
      backedge = phi.operands()[index];
    } else {
      if (!initial.empty())
        return std::nullopt;
      initial = phi.operands()[index];
    }
  }
  if (initial.empty() || backedge.empty())
    return std::nullopt;
  (void)function;
  return std::pair<std::string, std::string>{initial, backedge};
}

bool decrementByOne(const Instruction &instruction,
                    const std::string &induction) {
  if (instruction.opcode() == "sub" && instruction.operands().size() == 2)
    return instruction.operands()[0] == induction &&
           isInteger(instruction.operands()[1], 1);
  if (instruction.opcode() != "add" || instruction.operands().size() != 2)
    return false;
  return (instruction.operands()[0] == induction &&
          isInteger(instruction.operands()[1], -1)) ||
         (instruction.operands()[1] == induction &&
          isInteger(instruction.operands()[0], -1));
}

bool incrementByOne(const Instruction &instruction,
                    const std::string &induction) {
  if (instruction.opcode() != "add" || instruction.operands().size() != 2)
    return false;
  return (instruction.operands()[0] == induction &&
          isInteger(instruction.operands()[1], 1)) ||
         (instruction.operands()[1] == induction &&
          isInteger(instruction.operands()[0], 1));
}

std::unordered_set<std::size_t>
naturalLoopBlocks(const CFG &cfg, std::size_t header, std::size_t latch) {
  std::unordered_set<std::size_t> blocks{header, latch};
  std::deque<std::size_t> worklist;
  if (latch != header)
    worklist.push_back(latch);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto predecessor : cfg.predecessors[block]) {
      if (blocks.insert(predecessor).second && predecessor != header)
        worklist.push_back(predecessor);
    }
  }
  return blocks;
}

bool zeroComparison(const Instruction &condition, const std::string &value) {
  if (condition.opcode() != "icmp.ne" || condition.operands().size() != 2)
    return false;
  return (condition.operands()[0] == value &&
          isInteger(condition.operands()[1], 0)) ||
         (condition.operands()[1] == value &&
          isInteger(condition.operands()[0], 0));
}

std::optional<CountdownLoop> countdownLoop(const Function &function,
                                           const CFG &cfg,
                                           const DefinitionMap &definitions,
                                           std::size_t header) {
  const auto *terminator = function.blocks()[header].terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3)
    return std::nullopt;
  const auto condition = definitions.find(terminator->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.location.block != header)
    return std::nullopt;
  const auto body = cfg.index.find(terminator->operands()[1]);
  const auto exit = cfg.index.find(terminator->operands()[2]);
  if (body == cfg.index.end() || exit == cfg.index.end())
    return std::nullopt;

  for (const auto &instruction : function.blocks()[header].instructions()) {
    if (instruction.opcode() != "phi" || !instruction.hasResult() ||
        !zeroComparison(*condition->second.instruction, instruction.result()))
      continue;
    const auto incoming = phiIncoming(instruction, function, cfg, header);
    if (!incoming.has_value())
      continue;
    const auto backedgeDefinition = definitions.find(incoming->second);
    if (backedgeDefinition == definitions.end() ||
        !decrementByOne(*backedgeDefinition->second.instruction,
                        instruction.result()))
      continue;
    const auto latch = backedgeDefinition->second.location.block;
    const auto *latchTerminator = function.blocks()[latch].terminator();
    if (latchTerminator == nullptr || latchTerminator->opcode() != "br" ||
        latchTerminator->operands().size() != 1 ||
        latchTerminator->operands()[0] != function.blocks()[header].name())
      continue;
    const auto blocks = naturalLoopBlocks(cfg, header, latch);
    if (blocks.size() != 2 || blocks.find(body->second) == blocks.end())
      continue;
    std::size_t outsidePredecessors = 0;
    std::size_t preheader = 0;
    for (const auto predecessor : cfg.predecessors[header]) {
      if (blocks.find(predecessor) != blocks.end())
        continue;
      ++outsidePredecessors;
      preheader = predecessor;
    }
    if (outsidePredecessors != 1)
      continue;
    return CountdownLoop{header,
                         preheader,
                         latch,
                         exit->second,
                         instruction.result(),
                         incoming->first,
                         incoming->second,
                         blocks};
  }
  return std::nullopt;
}

bool recognizedTerminatingBackedge(const Function &function, const CFG &cfg,
                                   const DefinitionMap &definitions,
                                   std::size_t latch, std::size_t header) {
  const auto *terminator = function.blocks()[header].terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3)
    return false;
  const auto condition = definitions.find(terminator->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.location.block != header ||
      condition->second.instruction->operands().size() != 2)
    return false;
  const auto &comparison = *condition->second.instruction;
  for (const auto &instruction : function.blocks()[header].instructions()) {
    if (instruction.opcode() != "phi" || !instruction.hasResult())
      continue;
    const auto incoming = phiIncoming(instruction, function, cfg, header);
    if (!incoming.has_value())
      continue;
    const auto backedge = definitions.find(incoming->second);
    if (backedge == definitions.end() ||
        backedge->second.location.block != latch)
      continue;
    if (zeroComparison(comparison, instruction.result()) &&
        decrementByOne(*backedge->second.instruction, instruction.result()))
      return true;
    if (comparison.opcode() == "icmp.slt" &&
        comparison.operands()[0] == instruction.result() &&
        isInteger(incoming->first, 0) &&
        incrementByOne(*backedge->second.instruction, instruction.result())) {
      const auto bound = definitions.find(comparison.operands()[1]);
      if (bound == definitions.end() ||
          !dominates(cfg, header, bound->second.location.block))
        return true;
    }
  }
  return false;
}

bool acyclicWithoutBackedges(
    const CFG &cfg,
    const std::unordered_set<unsigned long long> &removedEdges) {
  std::vector<unsigned char> state(cfg.successors.size(), 0);
  std::function<bool(std::size_t)> visit = [&](std::size_t block) {
    if (state[block] == 1)
      return false;
    if (state[block] == 2)
      return true;
    state[block] = 1;
    for (const auto successor : cfg.successors[block]) {
      const auto edge = (static_cast<unsigned long long>(block) << 32U) |
                        static_cast<unsigned long long>(successor);
      if (removedEdges.find(edge) != removedEdges.end())
        continue;
      if (!visit(successor))
        return false;
    }
    state[block] = 2;
    return true;
  };
  for (std::size_t block = 0; block < cfg.successors.size(); ++block) {
    if (state[block] == 0 && !visit(block))
      return false;
  }
  return true;
}

bool functionProvablyTerminates(
    const Module &module, std::size_t functionIndex,
    const std::unordered_map<std::string, std::size_t> &functionIndices,
    std::vector<unsigned char> &state, std::vector<unsigned char> &result) {
  if (state[functionIndex] == 2)
    return result[functionIndex] != 0;
  if (state[functionIndex] == 1)
    return false;
  state[functionIndex] = 1;
  const auto &function = module.functions()[functionIndex];
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  std::unordered_set<unsigned long long> backedges;
  for (std::size_t block = 0; block < cfg.successors.size(); ++block) {
    for (const auto successor : cfg.successors[block]) {
      if (!dominates(cfg, successor, block))
        continue;
      if (!recognizedTerminatingBackedge(function, cfg, definitions, block,
                                         successor)) {
        state[functionIndex] = 2;
        result[functionIndex] = 0;
        return false;
      }
      backedges.insert((static_cast<unsigned long long>(block) << 32U) |
                       static_cast<unsigned long long>(successor));
    }
  }
  if (!acyclicWithoutBackedges(cfg, backedges)) {
    state[functionIndex] = 2;
    result[functionIndex] = 0;
    return false;
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "call" || instruction.operands().empty())
        continue;
      const auto callee = functionIndices.find(instruction.operands()[0]);
      if (callee == functionIndices.end() ||
          !functionProvablyTerminates(module, callee->second, functionIndices,
                                      state, result)) {
        state[functionIndex] = 2;
        result[functionIndex] = 0;
        return false;
      }
    }
  }
  state[functionIndex] = 2;
  result[functionIndex] = 1;
  return true;
}

bool hasUnsafeDivision(const Instruction &instruction) {
  if (instruction.opcode() != "sdiv" && instruction.opcode() != "udiv" &&
      instruction.opcode() != "srem" && instruction.opcode() != "urem" &&
      instruction.opcode() != "div" && instruction.opcode() != "mod")
    return false;
  if (instruction.operands().size() != 2)
    return true;
  long long divisor = 0;
  return !detail::parseIntegerLiteral(instruction.operands()[1], divisor) ||
         divisor == 0;
}

bool valueUsedInBlocks(const Function &function, const std::string &value,
                       const std::unordered_set<std::size_t> &blocks,
                       const Instruction *ignore = nullptr) {
  for (const auto block : blocks) {
    for (const auto &instruction : function.blocks()[block].instructions()) {
      if (&instruction == ignore)
        continue;
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    value) != instruction.operands().end())
        return true;
    }
  }
  return false;
}

bool resultEscapesLoop(const Function &function, const std::string &value,
                       const std::unordered_set<std::size_t> &blocks) {
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (blocks.find(block) != blocks.end())
      continue;
    for (const auto &instruction : function.blocks()[block].instructions()) {
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    value) != instruction.operands().end())
        return true;
    }
  }
  return false;
}

bool proveDeadPrefix(
    const Module &module, const Function &function, const CountdownLoop &loop,
    const CFG &cfg, const DefinitionMap &definitions,
    const FunctionEffectAnalysis &effects,
    const std::unordered_map<std::string, std::size_t> &functionIndices,
    std::vector<unsigned char> &terminationState,
    std::vector<unsigned char> &terminationResult) {
  std::size_t observedOverwritePhis = 0;
  for (const auto &instruction :
       function.blocks()[loop.header].instructions()) {
    if (instruction.opcode() != "phi" || !instruction.hasResult() ||
        instruction.result() == loop.induction)
      continue;
    const auto incoming = phiIncoming(instruction, function, cfg, loop.header);
    if (!incoming.has_value())
      return false;
    const auto backedge = definitions.find(incoming->second);
    if (backedge == definitions.end() ||
        backedge->second.location.block != loop.latch)
      return false;
    if (valueUsedInBlocks(function, instruction.result(), loop.blocks,
                          &instruction))
      return false;
    if (resultEscapesLoop(function, instruction.result(), loop.blocks))
      ++observedOverwritePhis;
  }
  if (observedOverwritePhis == 0)
    return false;

  bool hasProfitableCall = false;
  std::unordered_set<std::string> loopDefinitions;
  for (const auto block : loop.blocks) {
    for (const auto &instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult())
        loopDefinitions.insert(instruction.result());
      if (instruction.opcode() == "store" || instruction.opcode() == "ret" ||
          hasUnsafeDivision(instruction))
        return false;
      if (instruction.opcode() != "call")
        continue;
      if (instruction.operands().empty())
        return false;
      const auto calleeName = instruction.operands()[0];
      const auto callee = functionIndices.find(calleeName);
      if (callee == functionIndices.end())
        return false;
      const auto &effect = effects.effectFor(calleeName.substr(1));
      if (effect.writesExternalMemory || effect.hasOtherSideEffects)
        return false;
      if (!functionProvablyTerminates(module, callee->second, functionIndices,
                                      terminationState, terminationResult))
        return false;
      hasProfitableCall = true;
    }
  }
  if (!hasProfitableCall)
    return false;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (loop.blocks.find(block) != loop.blocks.end())
      continue;
    for (const auto &instruction : function.blocks()[block].instructions()) {
      for (const auto &operand : instruction.operands()) {
        if (loopDefinitions.find(operand) == loopDefinitions.end())
          continue;
        if (operand == loop.induction)
          continue;
        const auto definition = definitions.find(operand);
        if (definition != definitions.end() &&
            definition->second.location.block == loop.header &&
            definition->second.instruction->opcode() == "phi")
          continue;
        return false;
      }
    }
  }
  return true;
}

std::string freshName(const Function &function, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        names.insert(instruction.result());
    }
  }
  std::string candidate = "%" + stem;
  std::size_t suffix = 0;
  while (!names.insert(candidate).second)
    candidate = "%" + stem + "." + std::to_string(++suffix);
  return candidate;
}

bool rewriteInitialToLastIteration(Function &function,
                                   const CountdownLoop &loop) {
  auto &header = function.blocks()[loop.header];
  std::size_t phiIndex = header.instructions().size();
  std::size_t preheaderOperand = 0;
  for (std::size_t index = 0; index < header.instructions().size(); ++index) {
    const auto &instruction = header.instructions()[index];
    if (instruction.opcode() != "phi" || instruction.result() != loop.induction)
      continue;
    phiIndex = index;
    for (std::size_t index = 1; index + 1 < instruction.operands().size();
         index += 2) {
      if (instruction.operands()[index + 1] ==
          function.blocks()[loop.preheader].name()) {
        preheaderOperand = index;
        break;
      }
    }
  }
  if (phiIndex == header.instructions().size() || preheaderOperand == 0)
    return false;
  auto operands = header.instructions()[phiIndex].operands();
  long long literal = 0;
  if (detail::parseIntegerLiteral(loop.initial, literal)) {
    if (literal <= 1)
      return false;
    operands[preheaderOperand] = "1";
    header.instructions()[phiIndex] =
        Instruction{"phi", std::move(operands), loop.induction};
    return true;
  }
  auto &preheader = function.blocks()[loop.preheader];
  if (preheader.instructions().empty() || preheader.terminator() == nullptr)
    return false;
  const auto positive = freshName(function, "dead.prefix.positive");
  const auto selected = freshName(function, "dead.prefix.initial");
  auto insertion = preheader.instructions().end() - 1;
  insertion = preheader.instructions().insert(
      insertion, Instruction{"icmp.sgt", {loop.initial, "0"}, positive});
  preheader.instructions().insert(
      insertion + 1,
      Instruction{"select", {positive, "1", loop.initial}, selected});
  operands[preheaderOperand] = selected;
  header.instructions()[phiIndex] =
      Instruction{"phi", std::move(operands), loop.induction};
  return true;
}

} // namespace

void DeadPrefixIterationEliminationPass::run(Module &module) {
  FunctionEffectAnalysis effects(module);
  std::unordered_map<std::string, std::size_t> functionIndices;
  for (std::size_t index = 0; index < module.functions().size(); ++index)
    functionIndices.emplace("@" + module.functions()[index].name(), index);
  std::vector<unsigned char> terminationState(module.functions().size(), 0);
  std::vector<unsigned char> terminationResult(module.functions().size(), 0);
  std::size_t eliminated = 0;
  for (auto &function : module.functions()) {
    std::unordered_set<std::size_t> transformedHeaders;
    bool changed = true;
    while (changed) {
      changed = false;
      const auto cfg = buildCFG(function);
      const auto definitions = buildDefinitions(function);
      for (std::size_t header = 0; header < function.blocks().size();
           ++header) {
        if (transformedHeaders.find(header) != transformedHeaders.end())
          continue;
        const auto loop = countdownLoop(function, cfg, definitions, header);
        if (!loop.has_value() ||
            !proveDeadPrefix(module, function, *loop, cfg, definitions, effects,
                             functionIndices, terminationState,
                             terminationResult) ||
            !rewriteInitialToLastIteration(function, *loop))
          continue;
        ++eliminated;
        transformedHeaders.insert(header);
        changed = true;
        break;
      }
    }
  }
  if (std::getenv("COMPILER_DEAD_PREFIX_ITERATION_ELIMINATION_STATS") !=
      nullptr)
    std::cerr << "dead-prefix-iteration-elimination: eliminated=" << eliminated
              << '\n';
}

} // namespace compiler::midend::pass
