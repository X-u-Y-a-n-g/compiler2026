#include "../../../include/compiler/midend/pass/LoopInvariantGuardHoistingPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
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

struct Location {
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Definition {
  const Instruction* instruction = nullptr;
  Location location;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<std::vector<bool>> dominates;
};

struct Loop {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::size_t body = 0;
  std::size_t latch = 0;
  std::size_t exit = 0;
  std::unordered_set<std::size_t> blocks;
};

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") return {};
  if (terminator->operands().size() == 1) return {terminator->operands()[0]};
  if (terminator->operands().size() == 3) {
    return {terminator->operands()[1], terminator->operands()[2]};
  }
  return {};
}

CFG buildCFG(const Function& function) {
  CFG cfg;
  const auto count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.dominates.assign(count, std::vector<bool>(count, true));
  for (std::size_t block = 0; block < count; ++block) {
    cfg.index.emplace(function.blocks()[block].name(), block);
  }
  for (std::size_t block = 0; block < count; ++block) {
    for (const auto& target : branchTargets(function.blocks()[block])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end()) continue;
      cfg.successors[block].push_back(found->second);
      cfg.predecessors[found->second].push_back(block);
    }
  }
  if (count == 0) return cfg;
  std::fill(cfg.dominates[0].begin(), cfg.dominates[0].end(), false);
  cfg.dominates[0][0] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      std::vector<bool> next(count, true);
      if (cfg.predecessors[block].empty()) {
        std::fill(next.begin(), next.end(), false);
      } else {
        for (const auto predecessor : cfg.predecessors[block]) {
          for (std::size_t candidate = 0; candidate < count; ++candidate) {
            next[candidate] = next[candidate] &&
                              cfg.dominates[predecessor][candidate];
          }
        }
      }
      next[block] = true;
      if (next != cfg.dominates[block]) {
        cfg.dominates[block] = std::move(next);
        changed = true;
      }
    }
  }
  return cfg;
}

bool dominates(const CFG& cfg, std::size_t dominator, std::size_t block) {
  return block < cfg.dominates.size() && dominator < cfg.dominates.size() &&
         cfg.dominates[block][dominator];
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].hasResult()) {
        definitions.emplace(instructions[index].result(),
                            Definition{&instructions[index], {block, index}});
      }
    }
  }
  return definitions;
}

std::unordered_set<std::size_t> naturalLoop(const CFG& cfg,
                                            std::size_t header,
                                            std::size_t latch) {
  std::unordered_set<std::size_t> blocks{header, latch};
  std::vector<std::size_t> worklist;
  if (latch != header) worklist.push_back(latch);
  while (!worklist.empty()) {
    const auto block = worklist.back();
    worklist.pop_back();
    for (const auto predecessor : cfg.predecessors[block]) {
      if (blocks.insert(predecessor).second && predecessor != header) {
        worklist.push_back(predecessor);
      }
    }
  }
  return blocks;
}

std::vector<Loop> collectLoops(const Function& function, const CFG& cfg) {
  std::vector<Loop> loops;
  for (std::size_t latch = 0; latch < function.blocks().size(); ++latch) {
    for (const auto header : cfg.successors[latch]) {
      if (!dominates(cfg, header, latch)) continue;
      const auto blocks = naturalLoop(cfg, header, latch);
      const auto* terminator = function.blocks()[header].terminator();
      if (terminator == nullptr || terminator->opcode() != "br" ||
          terminator->operands().size() != 3) {
        continue;
      }
      const auto first = cfg.index.find(terminator->operands()[1]);
      const auto second = cfg.index.find(terminator->operands()[2]);
      if (first == cfg.index.end() || second == cfg.index.end()) continue;
      const bool firstInside = blocks.find(first->second) != blocks.end();
      const bool secondInside = blocks.find(second->second) != blocks.end();
      if (firstInside == secondInside) continue;
      std::size_t insidePredecessors = 0;
      for (const auto predecessor : cfg.predecessors[header]) {
        if (blocks.find(predecessor) != blocks.end()) {
          ++insidePredecessors;
          if (predecessor != latch) insidePredecessors = 2;
        }
      }
      if (insidePredecessors != 1) continue;
      std::size_t preheader = function.blocks().size();
      for (const auto predecessor : cfg.predecessors[header]) {
        if (blocks.find(predecessor) != blocks.end()) continue;
        if (preheader != function.blocks().size()) {
          preheader = function.blocks().size();
          break;
        }
        preheader = predecessor;
      }
      if (preheader == function.blocks().size()) continue;
      const auto* preheaderTerminator = function.blocks()[preheader].terminator();
      if (preheaderTerminator == nullptr ||
          preheaderTerminator->opcode() != "br" ||
          preheaderTerminator->operands().size() != 1 ||
          preheaderTerminator->operands()[0] != function.blocks()[header].name()) {
        continue;
      }
      const auto body = firstInside ? first->second : second->second;
      const auto exit = firstInside ? second->second : first->second;
      if (std::any_of(function.blocks()[exit].instructions().begin(),
                      function.blocks()[exit].instructions().end(),
                      [](const Instruction& instruction) {
                        return instruction.opcode() == "phi";
                      })) {
        continue;
      }
      loops.push_back(Loop{header, preheader, body, latch, exit, blocks});
    }
  }
  std::sort(loops.begin(), loops.end(), [](const Loop& lhs, const Loop& rhs) {
    return lhs.blocks.size() < rhs.blocks.size();
  });
  return loops;
}

bool isInteger(const std::string& value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

bool isPrivateAlloca(const std::string& value, const DefinitionMap& definitions,
                     const std::unordered_set<std::size_t>& loopBlocks) {
  const auto found = definitions.find(value);
  return found != definitions.end() && found->second.instruction != nullptr &&
         found->second.instruction->opcode() == "alloca" &&
         loopBlocks.find(found->second.location.block) == loopBlocks.end();
}

bool isNonEscapingAlloca(const Function& function, const std::string& value,
                         const DefinitionMap& definitions) {
  const auto found = definitions.find(value);
  if (found == definitions.end() || found->second.instruction == nullptr ||
      found->second.instruction->opcode() != "alloca") {
    return false;
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand != value) continue;
        if ((instruction.opcode() != "load" && instruction.opcode() != "store") ||
            instruction.operands().empty() ||
            instruction.operands().back() != value) return false;
      }
    }
  }
  return true;
}

bool noLoopStoreTo(const Function& function, const Loop& loop,
                   const std::string& pointer) {
  for (const auto block : loop.blocks) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.opcode() == "store" &&
          !instruction.operands().empty() &&
          instruction.operands().back() == pointer) {
        return false;
      }
    }
  }
  return true;
}

bool loopValuesDoNotEscape(const Function& function, const Loop& loop,
                           const DefinitionMap& definitions) {
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (loop.blocks.find(block) != loop.blocks.end()) continue;
    for (const auto& instruction : function.blocks()[block].instructions()) {
      for (const auto& operand : instruction.operands()) {
        const auto found = definitions.find(operand);
        if (found != definitions.end() &&
            loop.blocks.find(found->second.location.block) != loop.blocks.end()) {
          return false;
        }
      }
    }
  }
  return true;
}

bool proveMemoryInduction(const Function& function, const Loop& loop,
                          const DefinitionMap& definitions) {
  const auto* headerTerminator = function.blocks()[loop.header].terminator();
  if (headerTerminator == nullptr || headerTerminator->operands().size() != 3) {
    return false;
  }
  const auto condition = definitions.find(headerTerminator->operands()[0]);
  if (condition == definitions.end() || condition->second.instruction == nullptr ||
      condition->second.location.block != loop.header ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto load =
      definitions.find(condition->second.instruction->operands()[0]);
  if (load == definitions.end() || load->second.instruction == nullptr ||
      load->second.instruction->opcode() != "load" ||
      load->second.instruction->operands().size() != 2) {
    return false;
  }
  const std::string slot = load->second.instruction->operands().back();
  if (!isPrivateAlloca(slot, definitions, loop.blocks)) return false;
  const auto bound = definitions.find(condition->second.instruction->operands()[1]);
  if (bound != definitions.end() &&
      loop.blocks.find(bound->second.location.block) != loop.blocks.end()) {
    return false;
  }

  std::size_t initialStores = 0;
  std::size_t incrementStores = 0;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand != slot) continue;
        if (instruction.opcode() != "load" && instruction.opcode() != "store") {
          return false;
        }
      }
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3 ||
          instruction.operands().back() != slot) {
        continue;
      }
      if (loop.blocks.find(block) == loop.blocks.end()) {
        if (block != loop.preheader || !isInteger(instruction.operands()[1], 0)) {
          return false;
        }
        ++initialStores;
        continue;
      }
      if (block != loop.latch) return false;
      const auto update = definitions.find(instruction.operands()[1]);
      if (update == definitions.end() || update->second.instruction == nullptr ||
          update->second.location.block != loop.latch ||
          update->second.instruction->opcode() != "add" ||
          update->second.instruction->operands().size() != 2) {
        return false;
      }
      const auto& operands = update->second.instruction->operands();
      const std::string loaded = isInteger(operands[0], 1) ? operands[1] :
                                 isInteger(operands[1], 1) ? operands[0] : "";
      const auto prior = definitions.find(loaded);
      if (loaded.empty() || prior == definitions.end() ||
          prior->second.instruction == nullptr ||
          prior->second.location.block != loop.latch ||
          prior->second.instruction->opcode() != "load" ||
          prior->second.instruction->operands().size() != 2 ||
          prior->second.instruction->operands().back() != slot) {
        return false;
      }
      ++incrementStores;
    }
  }
  return initialStores == 1 && incrementStores == 1;
}

bool provePhiInduction(const Function& function, const Loop& loop,
                       const DefinitionMap& definitions) {
  const auto* headerTerminator = function.blocks()[loop.header].terminator();
  if (headerTerminator == nullptr || headerTerminator->operands().size() != 3) {
    return false;
  }
  const auto condition = definitions.find(headerTerminator->operands()[0]);
  if (condition == definitions.end() || condition->second.instruction == nullptr ||
      condition->second.location.block != loop.header ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto induction =
      definitions.find(condition->second.instruction->operands()[0]);
  if (induction == definitions.end() || induction->second.instruction == nullptr ||
      induction->second.location.block != loop.header ||
      induction->second.instruction->opcode() != "phi" ||
      induction->second.instruction->operands().size() != 5) {
    return false;
  }
  const auto bound = definitions.find(condition->second.instruction->operands()[1]);
  if (bound != definitions.end() &&
      loop.blocks.find(bound->second.location.block) != loop.blocks.end()) {
    return false;
  }
  std::string backedgeValue;
  bool sawInitial = false;
  const auto& phiOperands = induction->second.instruction->operands();
  for (std::size_t index = 1; index + 1 < phiOperands.size(); index += 2) {
    if (phiOperands[index + 1] == function.blocks()[loop.preheader].name()) {
      if (!isInteger(phiOperands[index], 0)) return false;
      sawInitial = true;
    } else if (phiOperands[index + 1] == function.blocks()[loop.latch].name()) {
      backedgeValue = phiOperands[index];
    } else {
      return false;
    }
  }
  const auto update = definitions.find(backedgeValue);
  if (!sawInitial || update == definitions.end() ||
      update->second.instruction == nullptr ||
      update->second.location.block != loop.latch ||
      update->second.instruction->opcode() != "add" ||
      update->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto& operands = update->second.instruction->operands();
  const auto& inductionName = induction->second.instruction->result();
  return (operands[0] == inductionName && isInteger(operands[1], 1)) ||
         (operands[1] == inductionName && isInteger(operands[0], 1));
}

bool isPureCloneableOpcode(const std::string& opcode) {
  return opcode == "add" || opcode == "sub" || opcode == "mul" ||
         opcode == "and" || opcode == "or" || opcode == "xor" ||
         opcode == "shl" || opcode == "lshr" || opcode == "ashr" ||
         opcode == "select" || opcode == "copy" || opcode == "zext" ||
         opcode == "sext" || opcode == "trunc" ||
         opcode.rfind("icmp.", 0) == 0;
}

std::unordered_set<std::string> collectNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  return names;
}

std::string freshName(std::unordered_set<std::string>& names,
                      const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = "%" + stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) return candidate;
  }
}

struct CloneContext {
  const Function& function;
  const Loop& loop;
  const DefinitionMap& definitions;
  std::unordered_set<std::string>& names;
  std::unordered_map<std::string, std::string> mapped;
  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> clonedOriginals;
  std::vector<Instruction> instructions;
};

std::optional<std::string> cloneInvariantValue(const std::string& value,
                                               CloneContext& context) {
  if (value.empty() || value.front() != '%') return value;
  const auto mapped = context.mapped.find(value);
  if (mapped != context.mapped.end()) return mapped->second;
  const auto found = context.definitions.find(value);
  if (found == context.definitions.end()) return value;
  if (context.loop.blocks.find(found->second.location.block) ==
      context.loop.blocks.end()) {
    return value;
  }
  if (!context.visiting.insert(value).second ||
      found->second.instruction == nullptr) {
    return std::nullopt;
  }
  const auto& instruction = *found->second.instruction;
  bool cloneable = isPureCloneableOpcode(instruction.opcode());
  if (instruction.opcode() == "load" && instruction.operands().size() == 2) {
    const auto& pointer = instruction.operands().back();
    cloneable = isPrivateAlloca(pointer, context.definitions,
                                context.loop.blocks) &&
                isNonEscapingAlloca(context.function, pointer,
                                    context.definitions) &&
                noLoopStoreTo(context.function, context.loop, pointer);
  }
  if (!cloneable) {
    context.visiting.erase(value);
    return std::nullopt;
  }
  std::vector<std::string> operands;
  operands.reserve(instruction.operands().size());
  for (const auto& operand : instruction.operands()) {
    const auto cloned = cloneInvariantValue(operand, context);
    if (!cloned.has_value()) {
      context.visiting.erase(value);
      return std::nullopt;
    }
    operands.push_back(*cloned);
  }
  const auto result = freshName(context.names, "hoisted.guard");
  context.instructions.emplace_back(instruction.opcode(), std::move(operands),
                                    result);
  context.mapped.emplace(value, result);
  context.clonedOriginals.insert(value);
  context.visiting.erase(value);
  return result;
}

bool blockContainsOnlyClonedConditionSlice(
    const BasicBlock& block,
    const std::unordered_set<std::string>& clonedOriginals) {
  if (block.instructions().empty()) return false;
  for (std::size_t index = 0; index + 1 < block.instructions().size(); ++index) {
    const auto& instruction = block.instructions()[index];
    if (!instruction.hasResult() ||
        clonedOriginals.find(instruction.result()) == clonedOriginals.end()) {
      return false;
    }
  }
  return true;
}

bool blockPrefixHasNoObservableEffects(const Function& function,
                                       const BasicBlock& block,
                                       const DefinitionMap& definitions) {
  if (block.instructions().empty()) return false;
  for (std::size_t index = 0; index + 1 < block.instructions().size(); ++index) {
    const auto& instruction = block.instructions()[index];
    if (instruction.opcode() == "store") {
      if (instruction.operands().size() != 3 ||
          !isNonEscapingAlloca(function, instruction.operands().back(),
                               definitions)) {
        return false;
      }
      continue;
    }
    if (instruction.opcode() == "call" || instruction.opcode() == "ret") {
      return false;
    }
    if (instruction.opcode() == "sdiv" || instruction.opcode() == "udiv" ||
        instruction.opcode() == "srem" || instruction.opcode() == "urem" ||
        instruction.opcode() == "div" || instruction.opcode() == "mod") {
      return false;
    }
  }
  return true;
}

bool transformOne(Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  auto names = collectNames(function);
  for (const auto& loop : collectLoops(function, cfg)) {
    if (!loopValuesDoNotEscape(function, loop, definitions) ||
        (!proveMemoryInduction(function, loop, definitions) &&
         !provePhiInduction(function, loop, definitions))) {
      continue;
    }
    std::size_t current = loop.body;
    std::vector<std::size_t> guardBlocks;
    std::vector<std::string> conditions;
    CloneContext clone{function, loop, definitions, names, {}, {}, {}, {}};
    while (loop.blocks.find(current) != loop.blocks.end()) {
      const auto* terminator = function.blocks()[current].terminator();
      if (terminator == nullptr || terminator->opcode() != "br" ||
          terminator->operands().size() != 3) {
        break;
      }
      const auto first = cfg.index.find(terminator->operands()[1]);
      const auto second = cfg.index.find(terminator->operands()[2]);
      if (first == cfg.index.end() || second == cfg.index.end()) break;
      std::size_t pass = function.blocks().size();
      bool invert = false;
      if (first->second == loop.latch && second->second != loop.latch) {
        pass = second->second;
        invert = true;
      } else if (second->second == loop.latch && first->second != loop.latch) {
        pass = first->second;
      } else {
        break;
      }
      const auto condition = cloneInvariantValue(terminator->operands()[0], clone);
      if (!condition.has_value()) break;
      std::string passingCondition = *condition;
      if (invert) {
        passingCondition = freshName(names, "hoisted.guard.not");
        clone.instructions.emplace_back(
            "xor", std::vector<std::string>{*condition, "1"}, passingCondition);
      }
      if (!guardBlocks.empty() &&
          !blockContainsOnlyClonedConditionSlice(
              function.blocks()[current], clone.clonedOriginals)) {
        break;
      }
      guardBlocks.push_back(current);
      conditions.push_back(std::move(passingCondition));
      current = pass;
    }
    if (guardBlocks.empty() || current == loop.latch ||
        !blockPrefixHasNoObservableEffects(
            function, function.blocks()[guardBlocks.front()], definitions)) {
      continue;
    }

    std::string combined = conditions.front();
    for (std::size_t index = 1; index < conditions.size(); ++index) {
      const auto next = freshName(names, "hoisted.guard.all");
      clone.instructions.emplace_back(
          "and", std::vector<std::string>{combined, conditions[index]}, next);
      combined = next;
    }
    auto& preheader = function.blocks()[loop.preheader].instructions();
    if (preheader.empty()) continue;
    preheader.insert(preheader.end() - 1, clone.instructions.begin(),
                     clone.instructions.end());
    preheader.back() = Instruction{
        "br",
        {combined, function.blocks()[loop.header].name(),
         function.blocks()[loop.exit].name()}};
    auto& firstGuard = function.blocks()[guardBlocks.front()].instructions();
    firstGuard.back() = Instruction{"br", {function.blocks()[current].name()}};
    return true;
  }
  return false;
}

}  // namespace

void LoopInvariantGuardHoistingPass::run(
    compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_LOOP_INVARIANT_GUARD_HOISTING") != nullptr) {
    return;
  }
  std::size_t transformed = 0;
  for (auto& function : module.functions()) {
    while (transformOne(function)) ++transformed;
  }
  if (transformed != 0) {
    module.addPassNote("hoisted loop-invariant guard chains out of " +
                       std::to_string(transformed) + " inner loop(s)");
  }
  if (std::getenv("COMPILER_LOOP_INVARIANT_GUARD_HOISTING_STATS") != nullptr) {
    std::cerr << "loop-invariant-guard-hoisting: transformed=" << transformed
              << '\n';
  }
}

}  // namespace compiler::midend::pass
