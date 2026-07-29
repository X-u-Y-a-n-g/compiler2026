#include "../../../include/compiler/midend/pass/GuardedInitializationSinkingPass.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <regex>
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

struct Candidate {
  std::string alloca;
  std::size_t extent = 0;
  std::size_t sink = 0;
  std::vector<std::size_t> entryInstructions;
};

bool isGep(const Instruction &instruction) {
  const auto &opcode = instruction.opcode();
  return opcode == "getelementptr" || opcode == "getelementptr.linear" ||
         opcode == "gep" || opcode == "gep.linear";
}

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  if (terminator->operands().size() == 1) {
    return {terminator->operands()[0]};
  }
  if (terminator->operands().size() == 3) {
    if (terminator->operands()[1] == terminator->operands()[2]) {
      return {terminator->operands()[1]};
    }
    return {terminator->operands()[1], terminator->operands()[2]};
  }
  return {};
}

CFG buildCFG(const Function &function) {
  CFG cfg;
  const auto count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.dominators.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < count; ++index) {
    for (const auto &target : branchTargets(function.blocks()[index])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end()) {
        continue;
      }
      cfg.successors[index].push_back(found->second);
      cfg.predecessors[found->second].push_back(index);
    }
  }
  if (count == 0) {
    return cfg;
  }
  cfg.dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    for (std::size_t candidate = 0; candidate < count; ++candidate) {
      cfg.dominators[block].insert(candidate);
    }
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
            if (other.find(*iterator) == other.end()) {
              iterator = next.erase(iterator);
            } else {
              ++iterator;
            }
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

bool dominates(const CFG &cfg, std::size_t first, std::size_t second) {
  return second < cfg.dominators.size() &&
         cfg.dominators[second].find(first) != cfg.dominators[second].end();
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

std::optional<std::size_t> staticArrayExtent(const Instruction &instruction) {
  if (instruction.opcode() != "alloca" || !instruction.hasResult() ||
      instruction.operands().size() != 1) {
    return std::nullopt;
  }
  static const std::regex pattern(R"(^[^\[\]]+\[([1-9][0-9]*)\]$)");
  std::smatch match;
  if (!std::regex_match(instruction.operands()[0], match, pattern)) {
    return std::nullopt;
  }
  try {
    const auto extent = std::stoull(match[1].str());
    if (extent == 0 || extent > 65536) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(extent);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::pair<std::string, std::string>>
gepAccess(const std::string &address, const DefinitionMap &definitions) {
  const auto found = definitions.find(address);
  if (found == definitions.end() || !isGep(*found->second.instruction) ||
      found->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  return std::pair{found->second.instruction->operands()[0],
                   found->second.instruction->operands()[1]};
}

std::unordered_set<std::string>
derivedPointers(const DefinitionMap &definitions, const std::string &alloca) {
  std::unordered_set<std::string> pointers{alloca};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[value, definition] : definitions) {
      if (isGep(*definition.instruction) &&
          definition.instruction->operands().size() == 2 &&
          pointers.find(definition.instruction->operands()[0]) !=
              pointers.end()) {
        changed |= pointers.insert(value).second;
      }
    }
  }
  return pointers;
}

bool doesNotEscape(const Function &function,
                   const std::unordered_set<std::string> &pointers) {
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      for (std::size_t index = 0; index < instruction.operands().size();
           ++index) {
        if (pointers.find(instruction.operands()[index]) == pointers.end()) {
          continue;
        }
        if (isGep(instruction) && index == 0) {
          continue;
        }
        if (instruction.opcode() == "load" && index == 1) {
          continue;
        }
        if (instruction.opcode() == "store" && index == 2) {
          continue;
        }
        return false;
      }
    }
  }
  return true;
}

std::unordered_map<std::string, std::size_t>
useCounts(const Function &function) {
  std::unordered_map<std::string, std::size_t> counts;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      for (const auto &operand : instruction.operands()) {
        if (!operand.empty() && operand.front() == '%') {
          ++counts[operand];
        }
      }
    }
  }
  return counts;
}

bool isLoopHeader(const CFG &cfg, std::size_t block) {
  for (const auto predecessor : cfg.predecessors[block]) {
    if (dominates(cfg, block, predecessor)) {
      return true;
    }
  }
  return false;
}

bool guardRegionOnlyReturnsOrReachesSink(const Function &function,
                                         const CFG &cfg, std::size_t sink) {
  if (function.blocks().empty() || sink == 0) {
    return false;
  }
  bool reachedSink = false;
  bool reachedReturn = false;
  std::unordered_set<std::size_t> visited;
  std::vector<std::size_t> work{0};
  while (!work.empty()) {
    const auto block = work.back();
    work.pop_back();
    if (block == sink) {
      reachedSink = true;
      continue;
    }
    if (!visited.insert(block).second) {
      continue;
    }
    const auto *terminator = function.blocks()[block].terminator();
    if (terminator == nullptr) {
      return false;
    }
    if (terminator->opcode() == "ret") {
      reachedReturn = true;
      continue;
    }
    if (terminator->opcode() != "br" || cfg.successors[block].empty()) {
      return false;
    }
    for (const auto successor : cfg.successors[block]) {
      work.push_back(successor);
    }
  }
  for (const auto block : visited) {
    for (const auto successor : cfg.successors[block]) {
      if (successor != sink && visited.find(successor) != visited.end() &&
          dominates(cfg, successor, block)) {
        return false;
      }
    }
  }
  return reachedSink && reachedReturn;
}

std::optional<Candidate>
findCandidateForAlloca(const Function &function, const CFG &cfg,
                       const DefinitionMap &definitions,
                       const std::unordered_map<std::string, std::size_t> &uses,
                       const std::string &alloca, std::size_t extent) {
  if (function.blocks().empty()) {
    return std::nullopt;
  }
  const auto pointers = derivedPointers(definitions, alloca);
  if (!doesNotEscape(function, pointers)) {
    return std::nullopt;
  }

  const auto &entry = function.blocks().front();
  std::vector<std::optional<std::size_t>> initialized(extent);
  std::unordered_set<std::size_t> moved;
  for (std::size_t index = 0; index < entry.instructions().size(); ++index) {
    const auto &instruction = entry.instructions()[index];
    if (instruction.opcode() != "store" || instruction.operands().size() != 3) {
      continue;
    }
    const auto access = gepAccess(instruction.operands()[2], definitions);
    if (!access.has_value() || access->first != alloca) {
      continue;
    }
    long long element = 0;
    if (!detail::parseIntegerLiteral(access->second, element) || element < 0 ||
        static_cast<std::size_t>(element) >= extent ||
        initialized[static_cast<std::size_t>(element)].has_value()) {
      return std::nullopt;
    }
    long long storedValue = 0;
    if (!detail::parseIntegerLiteral(instruction.operands()[1], storedValue)) {
      return std::nullopt;
    }
    const auto addressDefinition = definitions.find(instruction.operands()[2]);
    const auto addressUses = uses.find(instruction.operands()[2]);
    if (addressDefinition == definitions.end() ||
        addressDefinition->second.location.block != 0 ||
        addressUses == uses.end() || addressUses->second != 1) {
      return std::nullopt;
    }
    initialized[static_cast<std::size_t>(element)] = index;
    moved.insert(addressDefinition->second.location.instruction);
    moved.insert(index);
  }
  for (const auto &store : initialized) {
    if (!store.has_value()) {
      return std::nullopt;
    }
  }

  std::vector<std::size_t> accessBlocks;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto &instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (block == 0 && moved.find(index) != moved.end()) {
        continue;
      }
      const auto &instruction = instructions[index];
      bool accesses = false;
      for (const auto &operand : instruction.operands()) {
        if (pointers.find(operand) != pointers.end()) {
          accesses = true;
          break;
        }
      }
      if (accesses) {
        accessBlocks.push_back(block);
      }
    }
  }
  if (accessBlocks.empty()) {
    return std::nullopt;
  }

  std::optional<std::size_t> sink;
  std::size_t bestDepth = 0;
  for (std::size_t block = 1; block < function.blocks().size(); ++block) {
    if (isLoopHeader(cfg, block)) {
      continue;
    }
    const bool dominatesAll = std::all_of(
        accessBlocks.begin(), accessBlocks.end(), [&](std::size_t accessBlock) {
          return dominates(cfg, block, accessBlock);
        });
    if (!dominatesAll ||
        !guardRegionOnlyReturnsOrReachesSink(function, cfg, block)) {
      continue;
    }
    const auto depth = cfg.dominators[block].size();
    if (!sink.has_value() || depth > bestDepth) {
      sink = block;
      bestDepth = depth;
    }
  }
  if (!sink.has_value()) {
    return std::nullopt;
  }

  Candidate candidate{alloca, extent, *sink, {}};
  candidate.entryInstructions.assign(moved.begin(), moved.end());
  std::sort(candidate.entryInstructions.begin(),
            candidate.entryInstructions.end());
  return candidate;
}

bool sinkInitializers(Function &function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto uses = useCounts(function);
  std::vector<Candidate> candidates;
  for (const auto &[value, definition] : definitions) {
    const auto extent = staticArrayExtent(*definition.instruction);
    if (!extent.has_value()) {
      continue;
    }
    const auto candidate = findCandidateForAlloca(function, cfg, definitions,
                                                  uses, value, *extent);
    if (candidate.has_value()) {
      candidates.push_back(*candidate);
    }
  }
  if (candidates.empty()) {
    return false;
  }

  struct MovedInstruction {
    std::size_t originalIndex = 0;
    Instruction instruction{""};
  };
  std::unordered_set<std::size_t> removals;
  std::unordered_map<std::size_t, std::vector<MovedInstruction>> insertions;
  const auto &entryInstructions = function.blocks().front().instructions();
  for (const auto &candidate : candidates) {
    for (const auto index : candidate.entryInstructions) {
      removals.insert(index);
      insertions[candidate.sink].push_back(
          MovedInstruction{index, entryInstructions[index]});
    }
  }

  std::vector<Instruction> rewrittenEntry;
  rewrittenEntry.reserve(entryInstructions.size() - removals.size());
  for (std::size_t index = 0; index < entryInstructions.size(); ++index) {
    if (removals.find(index) == removals.end()) {
      rewrittenEntry.push_back(entryInstructions[index]);
    }
  }
  function.blocks().front().instructions() = std::move(rewrittenEntry);

  for (auto &[blockIndex, moved] : insertions) {
    std::sort(
        moved.begin(), moved.end(),
        [](const MovedInstruction &first, const MovedInstruction &second) {
          return first.originalIndex < second.originalIndex;
        });
    auto &instructions = function.blocks()[blockIndex].instructions();
    auto insertion = instructions.begin();
    while (insertion != instructions.end() && insertion->opcode() == "phi") {
      ++insertion;
    }
    std::vector<Instruction> values;
    values.reserve(moved.size());
    for (auto &item : moved) {
      values.push_back(std::move(item.instruction));
    }
    instructions.insert(insertion, values.begin(), values.end());
  }

  if (std::getenv("COMPILER_GUARDED_INITIALIZATION_SINKING_STATS") != nullptr) {
    std::size_t instructionCount = 0;
    for (const auto &candidate : candidates) {
      instructionCount += candidate.entryInstructions.size();
    }
    std::cerr << "guarded-initialization-sinking: " << function.name() << ": "
              << candidates.size() << " aggregate(s), " << instructionCount
              << " instruction(s)\n";
  }
  return true;
}

} // namespace

void GuardedInitializationSinkingPass::run(compiler::ir::core::Module &module) {
  for (auto &function : module.functions()) {
    sinkInitializers(function);
  }
}

} // namespace compiler::midend::pass
