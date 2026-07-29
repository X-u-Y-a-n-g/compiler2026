#include "../../../include/compiler/midend/pass/IdempotentIterationCollapsePass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iostream>
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

struct PointerPath {
  std::string base;
  std::vector<std::string> indices;
};

struct CanonicalLoop {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::size_t latch = 0;
  std::size_t exit = 0;
  std::string induction;
  std::string initial;
  std::string bound;
  std::string condition;
  std::unordered_set<std::size_t> blocks;
};

struct FunctionPointerSummary {
  bool valid = false;
  std::unordered_set<std::size_t> readParameters;
  std::unordered_set<std::size_t> writtenParameters;
  std::unordered_set<std::string> readGlobals;
  std::unordered_set<std::string> writtenGlobals;
  struct ParameterAccess {
    std::size_t parameter = 0;
    PointerPath path;
    Location location;
  };
  std::vector<ParameterAccess> parameterAccesses;
};

struct CopySeed {
  std::string destination;
  std::string source;
  std::vector<std::string> bounds;
  std::size_t completionBlock = 0;
};

struct Candidate {
  std::size_t function = 0;
  CanonicalLoop loop;
  bool dynamicBound = false;
};

bool isGepOpcode(const std::string &opcode) {
  return opcode == "getelementptr" || opcode == "getelementptr.linear" ||
         opcode == "gep" || opcode == "gep.linear";
}

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
  const auto &operands = terminator->operands();
  if (operands.size() == 1)
    return {operands[0]};
  if (operands.size() == 3) {
    if (operands[1] == operands[2])
      return {operands[1]};
    return {operands[1], operands[2]};
  }
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
        for (std::size_t index = 1; index < cfg.predecessors[block].size();
             ++index) {
          const auto &other = cfg.dominators[cfg.predecessors[block][index]];
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
      if (instructions[index].hasResult()) {
        definitions.emplace(instructions[index].result(),
                            Definition{&instructions[index], {block, index}});
      }
    }
  }
  return definitions;
}

std::optional<PointerPath>
pointerPath(const std::string &value, const DefinitionMap &definitions,
            std::unordered_set<std::string> &visiting) {
  if (!value.empty() && value.front() == '@')
    return PointerPath{value, {}};
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second)
    return std::nullopt;
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return PointerPath{value, {}};
  const auto &instruction = *found->second.instruction;
  if (instruction.opcode() == "alloca")
    return PointerPath{value, {}};
  if (instruction.opcode() == "cast" && !instruction.operands().empty())
    return pointerPath(instruction.operands().front(), definitions, visiting);
  if (!isGepOpcode(instruction.opcode()) || instruction.operands().size() != 2)
    return std::nullopt;
  auto path = pointerPath(instruction.operands()[0], definitions, visiting);
  if (!path.has_value())
    return std::nullopt;
  path->indices.push_back(instruction.operands()[1]);
  return path;
}

std::optional<PointerPath> pointerPath(const std::string &value,
                                       const DefinitionMap &definitions) {
  std::unordered_set<std::string> visiting;
  return pointerPath(value, definitions, visiting);
}

bool isInteger(const std::string &value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

std::optional<CanonicalLoop> canonicalLoop(const Function &function,
                                           const CFG &cfg,
                                           const DefinitionMap &definitions,
                                           std::size_t header) {
  const auto *terminator = function.blocks()[header].terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3)
    return std::nullopt;
  const auto conditionFound = definitions.find(terminator->operands()[0]);
  if (conditionFound == definitions.end() ||
      conditionFound->second.location.block != header ||
      conditionFound->second.instruction->opcode() != "icmp.slt" ||
      conditionFound->second.instruction->operands().size() != 2)
    return std::nullopt;
  const auto &induction = conditionFound->second.instruction->operands()[0];
  const auto &bound = conditionFound->second.instruction->operands()[1];
  const auto phiFound = definitions.find(induction);
  if (phiFound == definitions.end() ||
      phiFound->second.location.block != header ||
      phiFound->second.instruction->opcode() != "phi" ||
      phiFound->second.instruction->operands().size() != 5)
    return std::nullopt;
  const auto &phi = *phiFound->second.instruction;
  std::string preheaderName;
  std::string latchName;
  std::string initial;
  std::string backedge;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    const auto predecessor = cfg.index.find(phi.operands()[index + 1]);
    if (predecessor == cfg.index.end())
      return std::nullopt;
    if (dominates(cfg, header, predecessor->second)) {
      if (!latchName.empty())
        return std::nullopt;
      latchName = phi.operands()[index + 1];
      backedge = phi.operands()[index];
    } else {
      if (!preheaderName.empty())
        return std::nullopt;
      preheaderName = phi.operands()[index + 1];
      initial = phi.operands()[index];
    }
  }
  if (!isInteger(initial, 0) || preheaderName.empty() || latchName.empty())
    return std::nullopt;
  const auto preheader = cfg.index.find(preheaderName);
  const auto latch = cfg.index.find(latchName);
  const auto exit = cfg.index.find(terminator->operands()[2]);
  if (preheader == cfg.index.end() || latch == cfg.index.end() ||
      exit == cfg.index.end())
    return std::nullopt;
  const auto increment = definitions.find(backedge);
  if (increment == definitions.end() ||
      increment->second.instruction->opcode() != "add" ||
      increment->second.instruction->operands().size() != 2)
    return std::nullopt;
  const auto &add = increment->second.instruction->operands();
  if (!((add[0] == induction && isInteger(add[1], 1)) ||
        (add[1] == induction && isInteger(add[0], 1))))
    return std::nullopt;
  if (!dominates(cfg, header, increment->second.location.block) ||
      !dominates(cfg, increment->second.location.block, latch->second))
    return std::nullopt;
  CanonicalLoop loop;
  loop.header = header;
  loop.preheader = preheader->second;
  loop.latch = latch->second;
  loop.exit = exit->second;
  loop.induction = induction;
  loop.initial = initial;
  loop.bound = bound;
  loop.condition = conditionFound->second.instruction->result();
  loop.blocks.insert(header);
  loop.blocks.insert(latch->second);
  std::deque<std::size_t> worklist;
  if (latch->second != header)
    worklist.push_back(latch->second);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto predecessor : cfg.predecessors[block]) {
      if (loop.blocks.insert(predecessor).second && predecessor != header)
        worklist.push_back(predecessor);
    }
  }
  if (loop.blocks.find(preheader->second) != loop.blocks.end())
    return std::nullopt;
  std::size_t outsidePredecessors = 0;
  for (const auto predecessor : cfg.predecessors[header]) {
    if (loop.blocks.find(predecessor) == loop.blocks.end()) {
      ++outsidePredecessors;
      if (predecessor != preheader->second)
        return std::nullopt;
    }
  }
  if (outsidePredecessors != 1)
    return std::nullopt;
  for (const auto block : loop.blocks) {
    for (const auto successor : cfg.successors[block]) {
      if (loop.blocks.find(successor) == loop.blocks.end() &&
          !(block == header && successor == exit->second))
        return std::nullopt;
    }
  }
  return loop;
}

std::vector<CanonicalLoop> collectLoops(const Function &function,
                                        const CFG &cfg,
                                        const DefinitionMap &definitions) {
  std::vector<CanonicalLoop> loops;
  for (std::size_t header = 0; header < function.blocks().size(); ++header) {
    const auto loop = canonicalLoop(function, cfg, definitions, header);
    if (loop.has_value())
      loops.push_back(*loop);
  }
  return loops;
}

bool definitionInside(const std::string &value, const CanonicalLoop &loop,
                      const DefinitionMap &definitions) {
  if (value.empty() || value.front() != '%')
    return false;
  const auto found = definitions.find(value);
  return found != definitions.end() &&
         loop.blocks.find(found->second.location.block) != loop.blocks.end();
}

bool isLoopLocalAlloca(const std::string &base, const CanonicalLoop &loop,
                       const DefinitionMap &definitions) {
  const auto found = definitions.find(base);
  return found != definitions.end() &&
         found->second.instruction->opcode() == "alloca" &&
         loop.blocks.find(found->second.location.block) != loop.blocks.end();
}

bool inductionOnlyControlsLoop(const Function &function,
                               const CanonicalLoop &loop,
                               const DefinitionMap &definitions) {
  const auto phi = definitions.find(loop.induction);
  if (phi == definitions.end())
    return false;
  std::string increment;
  for (std::size_t index = 1;
       index + 1 < phi->second.instruction->operands().size(); index += 2) {
    const auto predecessor = function.blocks()[loop.latch].name();
    if (phi->second.instruction->operands()[index + 1] == predecessor)
      increment = phi->second.instruction->operands()[index];
  }
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto &instructions = function.blocks()[block].instructions();
    for (const auto &instruction : instructions) {
      for (const auto &operand : instruction.operands()) {
        if (operand != loop.induction)
          continue;
        if (instruction.result() == loop.condition ||
            instruction.result() == increment)
          continue;
        return false;
      }
    }
  }
  return true;
}

std::unordered_map<std::string, std::size_t>
parameterIndices(const Function &function) {
  std::unordered_map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < function.parameters().size(); ++index)
    result.emplace(function.parameters()[index].name, index);
  return result;
}

FunctionPointerSummary summarizeLeafFunction(const Function &function) {
  FunctionPointerSummary summary;
  const auto definitions = buildDefinitions(function);
  const auto parameters = parameterIndices(function);
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto &block = function.blocks()[blockIndex];
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions().size(); ++instructionIndex) {
      const auto &instruction = block.instructions()[instructionIndex];
      if (instruction.opcode() == "call")
        return summary;
      if (instruction.opcode() != "load" && instruction.opcode() != "store")
        continue;
      const auto &operands = instruction.operands();
      if ((instruction.opcode() == "load" && operands.size() != 2) ||
          (instruction.opcode() == "store" && operands.size() != 3))
        return summary;
      const auto path = pointerPath(operands.back(), definitions);
      if (!path.has_value())
        return summary;
      const auto parameter = parameters.find(path->base);
      const bool load = instruction.opcode() == "load";
      if (parameter != parameters.end()) {
        if (load)
          summary.readParameters.insert(parameter->second);
        else
          summary.writtenParameters.insert(parameter->second);
        summary.parameterAccesses.push_back(
            {parameter->second, *path, {blockIndex, instructionIndex}});
      } else if (!path->base.empty() && path->base.front() == '@') {
        if (load)
          summary.readGlobals.insert(path->base);
        else
          summary.writtenGlobals.insert(path->base);
      } else {
        const auto local = definitions.find(path->base);
        if (local == definitions.end() ||
            local->second.instruction->opcode() != "alloca")
          return summary;
      }
    }
  }
  summary.valid = true;
  return summary;
}

std::optional<std::string> externalBase(const std::string &address,
                                        const DefinitionMap &definitions,
                                        const CanonicalLoop &loop) {
  const auto path = pointerPath(address, definitions);
  if (!path.has_value())
    return std::nullopt;
  if (isLoopLocalAlloca(path->base, loop, definitions))
    return std::string{};
  return path->base;
}

bool allCallSitesProveNoAlias(
    const Module &module, std::size_t functionIndex,
    const std::unordered_set<std::size_t> &written,
    const std::unordered_set<std::size_t> &read,
    const std::unordered_set<std::string> &readGlobals,
    const std::unordered_set<std::string> &writtenGlobals) {
  const auto &callee = module.functions()[functionIndex];
  std::size_t calls = 0;
  for (std::size_t current = 0; current < module.functions().size();
       ++current) {
    const auto &caller = module.functions()[current];
    const auto definitions = buildDefinitions(caller);
    for (const auto &block : caller.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty() ||
            instruction.operands()[0] != "@" + callee.name())
          continue;
        ++calls;
        if (instruction.operands().size() != callee.parameters().size() + 1)
          return false;
        std::vector<std::string> bases(callee.parameters().size());
        for (std::size_t parameter = 0; parameter < bases.size(); ++parameter) {
          if (written.find(parameter) == written.end() &&
              read.find(parameter) == read.end())
            continue;
          const auto path =
              pointerPath(instruction.operands()[parameter + 1], definitions);
          if (!path.has_value() || path->base.empty())
            return false;
          bases[parameter] = path->base;
        }
        for (const auto destination : written) {
          if (destination >= bases.size() || bases[destination].empty())
            return false;
          for (const auto source : read) {
            if (source >= bases.size() || source == destination ||
                bases[source].empty())
              continue;
            if (bases[destination] == bases[source])
              return false;
          }
          if (readGlobals.find(bases[destination]) != readGlobals.end())
            return false;
        }
        for (const auto source : read) {
          if (source >= bases.size() || bases[source].empty())
            continue;
          if (writtenGlobals.find(bases[source]) != writtenGlobals.end())
            return false;
        }
      }
    }
  }
  return calls != 0;
}

bool proveWriteOnlyIteration(const Module &module, std::size_t functionIndex,
                             const Function &function,
                             const CanonicalLoop &loop,
                             const DefinitionMap &definitions) {
  std::unordered_set<std::string> readBases;
  std::unordered_set<std::string> writtenBases;
  std::unordered_set<std::size_t> readParameters;
  std::unordered_set<std::size_t> writtenParameters;
  std::unordered_set<std::string> readGlobals;
  std::unordered_set<std::string> writtenGlobals;
  const auto parameters = parameterIndices(function);
  std::size_t externalStores = 0;
  for (const auto block : loop.blocks) {
    for (const auto &instruction : function.blocks()[block].instructions()) {
      if (instruction.opcode() == "call" || instruction.opcode() == "ret")
        return false;
      if (instruction.opcode() != "load" && instruction.opcode() != "store")
        continue;
      const auto base =
          externalBase(instruction.operands().back(), definitions, loop);
      if (!base.has_value())
        return false;
      if (base->empty())
        continue;
      if (instruction.opcode() == "load") {
        readBases.insert(*base);
        const auto parameter = parameters.find(*base);
        if (parameter != parameters.end())
          readParameters.insert(parameter->second);
        else if (!base->empty() && base->front() == '@')
          readGlobals.insert(*base);
      } else {
        ++externalStores;
        writtenBases.insert(*base);
        const auto parameter = parameters.find(*base);
        if (parameter != parameters.end())
          writtenParameters.insert(parameter->second);
        else if (!base->empty() && base->front() == '@')
          writtenGlobals.insert(*base);
      }
    }
  }
  if (externalStores == 0)
    return false;
  for (const auto &base : writtenBases) {
    if (readBases.find(base) != readBases.end())
      return false;
  }
  if ((!writtenParameters.empty() ||
       (!writtenGlobals.empty() && !readParameters.empty())) &&
      !allCallSitesProveNoAlias(module, functionIndex, writtenParameters,
                                readParameters, readGlobals, writtenGlobals))
    return false;
  return true;
}

std::optional<const CanonicalLoop *>
loopForInduction(const std::vector<CanonicalLoop> &loops,
                 const std::string &induction, std::size_t block,
                 const CanonicalLoop &outer) {
  const CanonicalLoop *best = nullptr;
  for (const auto &loop : loops) {
    bool contained = true;
    for (const auto member : loop.blocks)
      contained &= outer.blocks.find(member) != outer.blocks.end();
    if (loop.induction != induction ||
        loop.blocks.find(block) == loop.blocks.end() || !contained)
      continue;
    if (best == nullptr || loop.blocks.size() < best->blocks.size())
      best = &loop;
  }
  return best == nullptr ? std::nullopt
                         : std::optional<const CanonicalLoop *>{best};
}

std::vector<CopySeed> collectCopySeeds(const Function &function, const CFG &cfg,
                                       const DefinitionMap &definitions,
                                       const std::vector<CanonicalLoop> &loops,
                                       const CanonicalLoop &outer) {
  std::vector<CopySeed> seeds;
  for (const auto block : outer.blocks) {
    const auto &instructions = function.blocks()[block].instructions();
    for (const auto &store : instructions) {
      if (store.opcode() != "store" || store.operands().size() != 3)
        continue;
      const auto destination = pointerPath(store.operands()[2], definitions);
      const auto value = definitions.find(store.operands()[1]);
      if (!destination.has_value() || value == definitions.end() ||
          value->second.instruction->opcode() != "load" ||
          value->second.instruction->operands().size() != 2)
        continue;
      const auto source =
          pointerPath(value->second.instruction->operands()[1], definitions);
      if (!source.has_value() || source->base == destination->base ||
          source->indices != destination->indices ||
          destination->indices.empty())
        continue;
      CopySeed seed;
      seed.destination = destination->base;
      seed.source = source->base;
      const CanonicalLoop *outermostSeedLoop = nullptr;
      std::vector<const CanonicalLoop *> dimensionLoops;
      bool valid = true;
      for (const auto &index : destination->indices) {
        const auto loop = loopForInduction(loops, index, block, outer);
        if (!loop.has_value() || !isInteger((*loop)->initial, 0) ||
            definitionInside((*loop)->bound, outer, definitions)) {
          valid = false;
          break;
        }
        seed.bounds.push_back((*loop)->bound);
        dimensionLoops.push_back(*loop);
        if (outermostSeedLoop == nullptr ||
            (*loop)->blocks.size() > outermostSeedLoop->blocks.size())
          outermostSeedLoop = *loop;
      }
      if (!valid || outermostSeedLoop == nullptr)
        continue;
      if (!dominates(cfg, block, dimensionLoops.back()->latch))
        continue;
      for (std::size_t dimension = 1; dimension < dimensionLoops.size();
           ++dimension) {
        if (!dominates(cfg, dimensionLoops[dimension]->preheader,
                       dimensionLoops[dimension - 1]->latch)) {
          valid = false;
          break;
        }
      }
      if (!valid)
        continue;
      seed.completionBlock = outermostSeedLoop->exit;
      seeds.push_back(std::move(seed));
    }
  }
  return seeds;
}

bool nonnegativeLoopInitial(const CanonicalLoop &loop,
                            const DefinitionMap &definitions,
                            const std::vector<CanonicalLoop> &loops,
                            unsigned depth = 0) {
  if (depth > 4)
    return false;
  long long literal = 0;
  if (detail::parseIntegerLiteral(loop.initial, literal))
    return literal >= 0;
  const auto found = definitions.find(loop.initial);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != "add" ||
      found->second.instruction->operands().size() != 2)
    return false;
  const auto &operands = found->second.instruction->operands();
  std::string base;
  if (detail::parseIntegerLiteral(operands[1], literal) && literal >= 0)
    base = operands[0];
  else if (detail::parseIntegerLiteral(operands[0], literal) && literal >= 0)
    base = operands[1];
  else
    return false;
  for (const auto &candidate : loops) {
    if (candidate.induction == base)
      return nonnegativeLoopInitial(candidate, definitions, loops, depth + 1);
  }
  return false;
}

bool isLoadFromSlot(const std::string &value, const std::string &slot,
                    const DefinitionMap &definitions) {
  const auto found = definitions.find(value);
  return found != definitions.end() &&
         found->second.instruction->opcode() == "load" &&
         found->second.instruction->operands().size() == 2 &&
         found->second.instruction->operands()[1] == slot;
}

bool proveNonnegativeValue(const std::string &value,
                           const DefinitionMap &definitions,
                           const std::vector<CanonicalLoop> &loops) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal))
    return literal >= 0;
  for (const auto &loop : loops) {
    if (loop.induction == value)
      return nonnegativeLoopInitial(loop, definitions, loops);
  }
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != "add" ||
      found->second.instruction->operands().size() != 2)
    return false;
  const auto &operands = found->second.instruction->operands();
  if (detail::parseIntegerLiteral(operands[1], literal) && literal >= 0)
    return proveNonnegativeValue(operands[0], definitions, loops);
  if (detail::parseIntegerLiteral(operands[0], literal) && literal >= 0)
    return proveNonnegativeValue(operands[1], definitions, loops);
  return false;
}

bool slotLoopCoversIndex(const Function &function, const CFG &cfg,
                         const DefinitionMap &definitions,
                         const std::vector<CanonicalLoop> &phiLoops,
                         const std::string &index, const std::string &bound,
                         const Location &access) {
  const auto indexDefinition = definitions.find(index);
  if (indexDefinition == definitions.end() ||
      indexDefinition->second.instruction->opcode() != "load" ||
      indexDefinition->second.instruction->operands().size() != 2)
    return false;
  const auto &slot = indexDefinition->second.instruction->operands()[1];
  const auto slotDefinition = definitions.find(slot);
  if (slotDefinition == definitions.end() ||
      slotDefinition->second.instruction->opcode() != "alloca")
    return false;

  for (std::size_t header = 0; header < function.blocks().size(); ++header) {
    const auto *terminator = function.blocks()[header].terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3)
      continue;
    const auto condition = definitions.find(terminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second.location.block != header ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2 ||
        condition->second.instruction->operands()[1] != bound ||
        !isLoadFromSlot(condition->second.instruction->operands()[0], slot,
                        definitions))
      continue;
    const auto body = cfg.index.find(terminator->operands()[1]);
    if (body == cfg.index.end() || !dominates(cfg, body->second, access.block))
      continue;
    std::vector<std::size_t> latches;
    for (const auto predecessor : cfg.predecessors[header]) {
      if (dominates(cfg, header, predecessor))
        latches.push_back(predecessor);
    }
    if (latches.size() != 1)
      continue;
    std::unordered_set<std::size_t> blocks{header, latches.front()};
    std::deque<std::size_t> worklist;
    if (latches.front() != header)
      worklist.push_back(latches.front());
    while (!worklist.empty()) {
      const auto block = worklist.front();
      worklist.pop_front();
      for (const auto predecessor : cfg.predecessors[block]) {
        if (blocks.insert(predecessor).second && predecessor != header)
          worklist.push_back(predecessor);
      }
    }
    if (blocks.find(access.block) == blocks.end())
      continue;

    const Instruction *initialStore = nullptr;
    Location initialLocation;
    bool valid = true;
    for (std::size_t block = 0; block < function.blocks().size(); ++block) {
      const auto &instructions = function.blocks()[block].instructions();
      for (std::size_t instructionIndex = 0;
           instructionIndex < instructions.size(); ++instructionIndex) {
        const auto &instruction = instructions[instructionIndex];
        if (instruction.opcode() != "store" ||
            instruction.operands().size() != 3 ||
            instruction.operands()[2] != slot)
          continue;
        const Location location{block, instructionIndex};
        if (blocks.find(block) == blocks.end()) {
          if (!dominates(cfg, block, header))
            continue;
          if (initialStore == nullptr ||
              dominates(cfg, initialLocation.block, block)) {
            initialStore = &instruction;
            initialLocation = location;
          }
          continue;
        }
        const auto valueDefinition =
            definitions.find(instruction.operands()[1]);
        if (valueDefinition == definitions.end() ||
            valueDefinition->second.instruction->opcode() != "add" ||
            valueDefinition->second.instruction->operands().size() != 2) {
          valid = false;
          break;
        }
        const auto &add = valueDefinition->second.instruction->operands();
        const bool increment =
            (isLoadFromSlot(add[0], slot, definitions) &&
             isInteger(add[1], 1)) ||
            (isLoadFromSlot(add[1], slot, definitions) && isInteger(add[0], 1));
        if (!increment ||
            (block == access.block && instructionIndex < access.instruction) ||
            (block != access.block && dominates(cfg, block, access.block))) {
          valid = false;
          break;
        }
      }
      if (!valid)
        break;
    }
    if (!valid || initialStore == nullptr ||
        !proveNonnegativeValue(initialStore->operands()[1], definitions,
                               phiLoops))
      continue;
    return true;
  }
  return false;
}

bool parameterAccessCovered(const Function &callee,
                            const FunctionPointerSummary &summary,
                            std::size_t parameter,
                            const std::vector<std::string> &expectedBounds,
                            const std::vector<std::string> &actualArguments) {
  const auto definitions = buildDefinitions(callee);
  const auto cfg = buildCFG(callee);
  const auto loops = collectLoops(callee, cfg, definitions);
  if (expectedBounds.empty())
    return false;
  std::vector<std::string> calleeBounds;
  for (const auto &bound : expectedBounds) {
    std::string mapped;
    long long literal = 0;
    if (detail::parseIntegerLiteral(bound, literal)) {
      mapped = bound;
    } else {
      for (std::size_t index = 0;
           index < actualArguments.size() && index < callee.parameters().size();
           ++index) {
        if (actualArguments[index] == bound)
          mapped = callee.parameters()[index].name;
      }
    }
    if (mapped.empty())
      return false;
    calleeBounds.push_back(mapped);
  }
  for (const auto &access : summary.parameterAccesses) {
    if (access.parameter != parameter)
      continue;
    const auto &path = access.path;
    if (path.indices.size() != calleeBounds.size())
      return false;
    for (std::size_t dimension = 0; dimension < path.indices.size();
         ++dimension) {
      bool covered = false;
      for (const auto &loop : loops) {
        if (loop.induction == path.indices[dimension] &&
            loop.bound == calleeBounds[dimension] &&
            loop.blocks.find(access.location.block) != loop.blocks.end() &&
            nonnegativeLoopInitial(loop, definitions, loops)) {
          const auto accessDefinition =
              definitions.find(path.indices[dimension]);
          (void)accessDefinition;
          covered = true;
          break;
        }
      }
      if (!covered)
        covered = slotLoopCoversIndex(callee, cfg, definitions, loops,
                                      path.indices[dimension],
                                      calleeBounds[dimension], access.location);
      if (!covered)
        return false;
    }
  }
  return true;
}

bool proveSeededCallIteration(
    const Module &module, std::size_t functionIndex, const Function &function,
    const CFG &cfg, const DefinitionMap &definitions,
    const std::vector<CanonicalLoop> &loops, const CanonicalLoop &outer,
    const std::unordered_map<std::string, std::size_t> &functionIndices,
    const std::vector<FunctionPointerSummary> &summaries) {
  const auto seeds = collectCopySeeds(function, cfg, definitions, loops, outer);
  if (seeds.empty())
    return false;
  std::unordered_map<std::string, CopySeed> seedForDestination;
  for (const auto &seed : seeds)
    seedForDestination.emplace(seed.destination, seed);
  std::unordered_set<std::string> readBases;
  std::unordered_set<std::string> writtenBases;
  std::vector<std::pair<std::size_t, const Instruction *>> calls;
  for (const auto block : outer.blocks) {
    const auto &instructions = function.blocks()[block].instructions();
    for (const auto &instruction : instructions) {
      if (instruction.opcode() == "ret")
        return false;
      if (instruction.opcode() == "load" || instruction.opcode() == "store") {
        const auto base =
            externalBase(instruction.operands().back(), definitions, outer);
        if (!base.has_value())
          return false;
        if (base->empty())
          continue;
        if (instruction.opcode() == "load")
          readBases.insert(*base);
        else
          writtenBases.insert(*base);
        continue;
      }
      if (instruction.opcode() != "call")
        continue;
      if (instruction.operands().empty())
        return false;
      const auto found = functionIndices.find(instruction.operands()[0]);
      if (found == functionIndices.end() || !summaries[found->second].valid)
        return false;
      const auto &callee = module.functions()[found->second];
      const auto &summary = summaries[found->second];
      if (instruction.operands().size() != callee.parameters().size() + 1)
        return false;
      std::vector<std::string> actualArguments(
          instruction.operands().begin() + 1, instruction.operands().end());
      for (const auto &argument : actualArguments) {
        if (definitionInside(argument, outer, definitions))
          return false;
      }
      for (const auto parameter : summary.readParameters) {
        const auto path = pointerPath(actualArguments[parameter], definitions);
        if (!path.has_value())
          return false;
        readBases.insert(path->base);
      }
      for (const auto parameter : summary.writtenParameters) {
        const auto path = pointerPath(actualArguments[parameter], definitions);
        if (!path.has_value())
          return false;
        writtenBases.insert(path->base);
      }
      readBases.insert(summary.readGlobals.begin(), summary.readGlobals.end());
      writtenBases.insert(summary.writtenGlobals.begin(),
                          summary.writtenGlobals.end());
      calls.emplace_back(block, &instruction);
    }
  }
  if (calls.empty())
    return false;
  for (const auto &base : readBases) {
    if (!base.empty() && base.front() == '%' &&
        (definitions.find(base) == definitions.end() ||
         definitions.at(base).instruction->opcode() != "alloca"))
      return false;
  }
  for (const auto &base : writtenBases) {
    if (!base.empty() && base.front() == '%' &&
        (definitions.find(base) == definitions.end() ||
         definitions.at(base).instruction->opcode() != "alloca"))
      return false;
  }
  for (const auto &base : writtenBases) {
    if (readBases.find(base) == readBases.end())
      continue;
    const auto seed = seedForDestination.find(base);
    if (seed == seedForDestination.end()) {
      return false;
    }
    if (writtenBases.find(seed->second.source) != writtenBases.end())
      return false;
    for (const auto &[block, call] : calls) {
      const auto found = functionIndices.find(call->operands()[0]);
      const auto &callee = module.functions()[found->second];
      const auto &summary = summaries[found->second];
      std::vector<std::string> actualArguments(call->operands().begin() + 1,
                                               call->operands().end());
      for (const auto parameter : summary.readParameters) {
        const auto path = pointerPath(actualArguments[parameter], definitions);
        if (!path.has_value() || path->base != base)
          continue;
        if (!dominates(cfg, seed->second.completionBlock, block) ||
            !parameterAccessCovered(callee, summary, parameter,
                                    seed->second.bounds, actualArguments)) {
          return false;
        }
      }
      for (const auto parameter : summary.writtenParameters) {
        const auto path = pointerPath(actualArguments[parameter], definitions);
        if (!path.has_value() || path->base != base)
          continue;
        if (!dominates(cfg, seed->second.completionBlock, block) ||
            !parameterAccessCovered(callee, summary, parameter,
                                    seed->second.bounds, actualArguments)) {
          return false;
        }
      }
    }
  }
  for (const auto &destination : writtenBases) {
    for (const auto &source : readBases) {
      if (destination == source)
        continue;
      if (!destination.empty() && destination.front() == '%' &&
          !source.empty() && source.front() == '%' && destination == source)
        return false;
    }
  }
  (void)functionIndex;
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

bool collapseToAtMostOne(Function &function, const Candidate &candidate) {
  auto &header = function.blocks()[candidate.loop.header];
  auto conditionIndex = header.instructions().size();
  for (std::size_t index = 0; index < header.instructions().size(); ++index) {
    if (header.instructions()[index].result() == candidate.loop.condition) {
      conditionIndex = index;
      break;
    }
  }
  if (conditionIndex == header.instructions().size())
    return false;
  auto condition = header.instructions()[conditionIndex];
  if (condition.opcode() != "icmp.slt" || condition.operands().size() != 2)
    return false;
  long long constantBound = 0;
  if (detail::parseIntegerLiteral(candidate.loop.bound, constantBound)) {
    if (constantBound <= 1)
      return false;
    condition = Instruction{
        "icmp.slt", {candidate.loop.induction, "1"}, condition.result()};
    header.instructions()[conditionIndex] = std::move(condition);
    return true;
  }
  const auto positive = freshName(function, "idempotent.trip.positive");
  const auto limit = freshName(function, "idempotent.trip.limit");
  std::vector<Instruction> rewritten;
  rewritten.reserve(header.instructions().size() + 2);
  for (std::size_t index = 0; index < header.instructions().size(); ++index) {
    if (index != conditionIndex) {
      rewritten.push_back(header.instructions()[index]);
      continue;
    }
    rewritten.emplace_back("icmp.sgt",
                           std::vector<std::string>{candidate.loop.bound, "0"},
                           positive);
    rewritten.emplace_back("select",
                           std::vector<std::string>{positive, "1", "0"}, limit);
    rewritten.emplace_back(
        "icmp.slt", std::vector<std::string>{candidate.loop.induction, limit},
        candidate.loop.condition);
  }
  header.instructions() = std::move(rewritten);
  return true;
}

} // namespace

void IdempotentIterationCollapsePass::run(Module &module) {
  std::unordered_map<std::string, std::size_t> functionIndices;
  for (std::size_t index = 0; index < module.functions().size(); ++index)
    functionIndices.emplace("@" + module.functions()[index].name(), index);
  std::vector<FunctionPointerSummary> summaries;
  summaries.reserve(module.functions().size());
  for (const auto &function : module.functions())
    summaries.push_back(summarizeLeafFunction(function));

  std::size_t writeOnly = 0;
  std::size_t seededCalls = 0;
  std::size_t canonical = 0;
  std::size_t controlOnly = 0;
  for (std::size_t functionIndex = 0; functionIndex < module.functions().size();
       ++functionIndex) {
    auto &function = module.functions()[functionIndex];
    bool changed = true;
    while (changed) {
      changed = false;
      const auto cfg = buildCFG(function);
      const auto definitions = buildDefinitions(function);
      const auto loops = collectLoops(function, cfg, definitions);
      canonical += loops.size();
      std::vector<CanonicalLoop> ordered = loops;
      std::sort(ordered.begin(), ordered.end(),
                [](const CanonicalLoop &lhs, const CanonicalLoop &rhs) {
                  return lhs.blocks.size() > rhs.blocks.size();
                });
      for (const auto &loop : ordered) {
        if (!inductionOnlyControlsLoop(function, loop, definitions) ||
            definitionInside(loop.bound, loop, definitions))
          continue;
        ++controlOnly;
        bool provenWriteOnly = proveWriteOnlyIteration(
            module, functionIndex, function, loop, definitions);
        bool provenSeeded = false;
        if (!provenWriteOnly) {
          provenSeeded = proveSeededCallIteration(
              module, functionIndex, function, cfg, definitions, loops, loop,
              functionIndices, summaries);
        }
        if (!provenWriteOnly && !provenSeeded)
          continue;
        if (!collapseToAtMostOne(function,
                                 Candidate{functionIndex, loop, true}))
          continue;
        writeOnly += provenWriteOnly ? 1 : 0;
        seededCalls += provenSeeded ? 1 : 0;
        changed = true;
        break;
      }
    }
  }
  if (std::getenv("COMPILER_IDEMPOTENT_ITERATION_COLLAPSE_STATS") != nullptr) {
    std::cerr << "idempotent-iteration-collapse: write_only=" << writeOnly
              << " seeded_calls=" << seededCalls << " canonical=" << canonical
              << " control_only=" << controlOnly << '\n';
  }
}

} // namespace compiler::midend::pass
