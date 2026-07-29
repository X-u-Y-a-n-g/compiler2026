#include "../../../include/compiler/midend/pass/StackAggregateInitPruningPass.h"

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

  bool operator==(const Location &other) const {
    return block == other.block && instruction == other.instruction;
  }
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

struct Access {
  std::string root;
  std::string index;
};

struct Candidate {
  std::string alloca;
  std::size_t extent = 0;
  std::vector<Location> initializationStores;
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
  static const std::regex llvmPattern(R"(^\[([1-9][0-9]*) x .+\]$)");
  static const std::regex internalPattern(R"(^[^\[\]]+\[([1-9][0-9]*)\]$)");
  std::smatch match;
  if (!std::regex_match(instruction.operands()[0], match, llvmPattern) &&
      !std::regex_match(instruction.operands()[0], match, internalPattern)) {
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

std::optional<Access> accessFor(const std::string &address,
                                const DefinitionMap &definitions) {
  const auto found = definitions.find(address);
  if (found == definitions.end() || !isGep(*found->second.instruction) ||
      found->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  return Access{found->second.instruction->operands()[0],
                found->second.instruction->operands()[1]};
}

bool allocaDoesNotEscape(const Function &function,
                         const DefinitionMap &definitions,
                         const std::string &alloca) {
  std::unordered_set<std::string> pointers{alloca};
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto &[value, definition] : definitions) {
      if (!isGep(*definition.instruction) ||
          definition.instruction->operands().size() != 2 ||
          pointers.find(definition.instruction->operands()[0]) ==
              pointers.end()) {
        continue;
      }
      grew |= pointers.insert(value).second;
    }
  }

  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &operands = instruction.operands();
      for (std::size_t index = 0; index < operands.size(); ++index) {
        if (pointers.find(operands[index]) == pointers.end()) {
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

bool isLoadFrom(const Instruction &instruction, const std::string &address) {
  return instruction.opcode() == "load" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         instruction.operands()[1] == address;
}

bool isStoreTo(const Instruction &instruction, const std::string &address) {
  return instruction.opcode() == "store" &&
         instruction.operands().size() == 3 &&
         instruction.operands()[2] == address;
}

bool isConstant(const std::string &value, long long expected) {
  long long literal = 0;
  return detail::parseIntegerLiteral(value, literal) && literal == expected;
}

bool isCurrentIndex(const std::string &value, const std::string &inductionValue,
                    const std::optional<std::string> &inductionSlot,
                    const DefinitionMap &definitions) {
  if (value == inductionValue) {
    return true;
  }
  if (!inductionSlot.has_value()) {
    return false;
  }
  const auto found = definitions.find(value);
  return found != definitions.end() &&
         isLoadFrom(*found->second.instruction, *inductionSlot);
}

bool isPreviousIndex(const std::string &value,
                     const std::string &inductionValue,
                     const std::optional<std::string> &inductionSlot,
                     const DefinitionMap &definitions) {
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != "sub" ||
      found->second.instruction->operands().size() != 2 ||
      !isConstant(found->second.instruction->operands()[1], 1)) {
    return false;
  }
  return isCurrentIndex(found->second.instruction->operands()[0],
                        inductionValue, inductionSlot, definitions);
}

std::optional<Location> findInitialStoreSet(const Function &function,
                                            const DefinitionMap &definitions,
                                            const std::string &alloca,
                                            std::size_t extent,
                                            std::vector<Location> &stores) {
  if (function.blocks().empty()) {
    return std::nullopt;
  }
  std::vector<std::optional<Location>> byIndex(extent);
  const auto &entry = function.blocks().front();
  for (std::size_t index = 0; index < entry.instructions().size(); ++index) {
    const auto &instruction = entry.instructions()[index];
    if (instruction.opcode() != "store" || instruction.operands().size() != 3) {
      continue;
    }
    const auto access = accessFor(instruction.operands()[2], definitions);
    if (!access.has_value() || access->root != alloca) {
      continue;
    }
    long long element = 0;
    if (!detail::parseIntegerLiteral(access->index, element) || element < 0 ||
        static_cast<std::size_t>(element) >= extent) {
      return std::nullopt;
    }
    if (byIndex[static_cast<std::size_t>(element)].has_value()) {
      continue;
    }
    byIndex[static_cast<std::size_t>(element)] = Location{0, index};
  }
  for (const auto &location : byIndex) {
    if (!location.has_value()) {
      return std::nullopt;
    }
    stores.push_back(*location);
  }
  return stores.empty() ? std::nullopt : std::optional<Location>{stores.back()};
}

bool proveCompleteOverwrite(const Function &function, const CFG &cfg,
                            const DefinitionMap &definitions,
                            const Candidate &candidate) {
  for (std::size_t headerIndex = 0; headerIndex < function.blocks().size();
       ++headerIndex) {
    const auto &header = function.blocks()[headerIndex];
    const auto *terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto bodyFound = cfg.index.find(terminator->operands()[1]);
    const auto exitFound = cfg.index.find(terminator->operands()[2]);
    if (bodyFound == cfg.index.end() || exitFound == cfg.index.end()) {
      continue;
    }
    const auto bodyIndex = bodyFound->second;
    const auto exitIndex = exitFound->second;
    const auto &body = function.blocks()[bodyIndex];
    if (body.terminator() == nullptr ||
        body.terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        cfg.predecessors[headerIndex].size() != 2 ||
        cfg.successors[bodyIndex] != std::vector<std::size_t>{headerIndex}) {
      continue;
    }

    const auto condition = definitions.find(terminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second.location.block != headerIndex ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2 ||
        !isConstant(condition->second.instruction->operands()[1],
                    static_cast<long long>(candidate.extent))) {
      continue;
    }
    std::size_t preheaderIndex = function.blocks().size();
    for (const auto predecessor : cfg.predecessors[headerIndex]) {
      if (predecessor != bodyIndex) {
        preheaderIndex = predecessor;
      }
    }
    if (preheaderIndex == function.blocks().size() ||
        function.blocks()[preheaderIndex].terminator() == nullptr ||
        function.blocks()[preheaderIndex].terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const auto &inductionValue = condition->second.instruction->operands()[0];
    const auto inductionDefinition = definitions.find(inductionValue);
    if (inductionDefinition == definitions.end() ||
        inductionDefinition->second.location.block != headerIndex) {
      continue;
    }
    std::optional<std::string> inductionSlot;
    bool phiInduction = false;
    if (inductionDefinition->second.instruction->opcode() == "load" &&
        inductionDefinition->second.instruction->operands().size() == 2) {
      inductionSlot = inductionDefinition->second.instruction->operands()[1];
      const auto inductionAlloca = definitions.find(*inductionSlot);
      if (inductionAlloca == definitions.end() ||
          inductionAlloca->second.instruction->opcode() != "alloca" ||
          staticArrayExtent(*inductionAlloca->second.instruction).has_value() ||
          *inductionSlot == candidate.alloca) {
        continue;
      }
    } else if (inductionDefinition->second.instruction->opcode() == "phi") {
      phiInduction = true;
    } else {
      continue;
    }

    bool startsAtOne = false;
    bool hasValidBackedgeIncrement = false;
    bool sawPreheaderInductionStore = phiInduction;
    if (phiInduction) {
      const auto &operands =
          inductionDefinition->second.instruction->operands();
      if (operands.size() < 5 || operands.size() % 2 == 0) {
        continue;
      }
      for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
        const auto &incoming = operands[index];
        const auto predecessor = cfg.index.find(operands[index + 1]);
        if (predecessor == cfg.index.end()) {
          continue;
        }
        if (predecessor->second == preheaderIndex) {
          startsAtOne = isConstant(incoming, 1);
        } else if (predecessor->second == bodyIndex) {
          const auto increment = definitions.find(incoming);
          hasValidBackedgeIncrement =
              increment != definitions.end() &&
              increment->second.location.block == bodyIndex &&
              increment->second.instruction->opcode() == "add" &&
              increment->second.instruction->operands().size() == 2 &&
              isConstant(increment->second.instruction->operands()[1], 1) &&
              isCurrentIndex(increment->second.instruction->operands()[0],
                             inductionValue, inductionSlot, definitions);
        }
      }
    }
    std::optional<Location> elementZeroStore;
    std::unordered_set<std::size_t> initialEntryStores;
    for (const auto &location : candidate.initializationStores) {
      if (location.block == 0) {
        initialEntryStores.insert(location.instruction);
      }
    }
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      const auto &instructions = function.blocks()[blockIndex].instructions();
      for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto &instruction = instructions[index];
        if (inductionSlot.has_value() && blockIndex == preheaderIndex &&
            isStoreTo(instruction, *inductionSlot)) {
          sawPreheaderInductionStore = true;
          startsAtOne = isConstant(instruction.operands()[1], 1);
        }
        if (instruction.opcode() != "store" ||
            instruction.operands().size() != 3) {
          continue;
        }
        const auto access = accessFor(instruction.operands()[2], definitions);
        if (access.has_value() && access->root == candidate.alloca &&
            isConstant(access->index, 0) &&
            dominates(cfg, blockIndex, headerIndex)) {
          if (blockIndex != 0 ||
              initialEntryStores.find(index) == initialEntryStores.end()) {
            elementZeroStore = Location{blockIndex, index};
          }
        }
      }
    }
    if (!sawPreheaderInductionStore || !startsAtOne ||
        (phiInduction && !hasValidBackedgeIncrement) ||
        !elementZeroStore.has_value()) {
      continue;
    }

    bool storesCurrent = false;
    bool increments = false;
    bool sawInductionStore = false;
    bool invalidBodyAccess = false;
    for (std::size_t index = 0; index + 1 < body.instructions().size();
         ++index) {
      const auto &instruction = body.instructions()[index];
      if (instruction.opcode() == "store" &&
          instruction.operands().size() == 3) {
        const auto access = accessFor(instruction.operands()[2], definitions);
        if (access.has_value() && access->root == candidate.alloca) {
          if (!isCurrentIndex(access->index, inductionValue, inductionSlot,
                              definitions) ||
              storesCurrent) {
            invalidBodyAccess = true;
            break;
          }
          storesCurrent = true;
        }
        if (inductionSlot.has_value() &&
            instruction.operands()[2] == *inductionSlot) {
          if (sawInductionStore) {
            invalidBodyAccess = true;
            break;
          }
          sawInductionStore = true;
          const auto increment = definitions.find(instruction.operands()[1]);
          if (increment != definitions.end() &&
              increment->second.instruction->opcode() == "add" &&
              increment->second.instruction->operands().size() == 2 &&
              isConstant(increment->second.instruction->operands()[1], 1) &&
              isCurrentIndex(increment->second.instruction->operands()[0],
                             inductionValue, inductionSlot, definitions)) {
            increments = true;
          }
        }
      }
      if (instruction.opcode() == "load" &&
          instruction.operands().size() == 2) {
        const auto access = accessFor(instruction.operands()[1], definitions);
        if (access.has_value() && access->root == candidate.alloca &&
            !isPreviousIndex(access->index, inductionValue, inductionSlot,
                             definitions)) {
          invalidBodyAccess = true;
          break;
        }
      }
    }
    if (!storesCurrent ||
        (!phiInduction && (!sawInductionStore || !increments)) ||
        invalidBodyAccess) {
      continue;
    }

    bool invalidLoad = false;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      if (blockIndex == bodyIndex) {
        continue;
      }
      for (const auto &instruction :
           function.blocks()[blockIndex].instructions()) {
        if (instruction.opcode() != "load" ||
            instruction.operands().size() != 2) {
          continue;
        }
        const auto access = accessFor(instruction.operands()[1], definitions);
        if (access.has_value() && access->root == candidate.alloca &&
            !dominates(cfg, exitIndex, blockIndex)) {
          invalidLoad = true;
          break;
        }
      }
      if (invalidLoad) {
        break;
      }
    }
    if (!invalidLoad) {
      return true;
    }
  }
  return false;
}

bool pruneFunction(Function &function) {
  const bool trace =
      std::getenv("COMPILER_STACK_AGGREGATE_INIT_PRUNING_STATS") != nullptr;
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  std::vector<Candidate> candidates;
  for (const auto &[value, definition] : definitions) {
    const auto extent = staticArrayExtent(*definition.instruction);
    if (!extent.has_value()) {
      continue;
    }
    if (!allocaDoesNotEscape(function, definitions, value)) {
      if (trace) {
        std::cerr << "stack-aggregate-init-pruning: " << function.name()
                  << ": rejected nonlocal pointer use\n";
      }
      continue;
    }
    Candidate candidate{value, *extent, {}};
    const bool initialized =
        findInitialStoreSet(function, definitions, value, *extent,
                            candidate.initializationStores)
            .has_value();
    const bool overwritten =
        initialized &&
        proveCompleteOverwrite(function, cfg, definitions, candidate);
    if (initialized && overwritten) {
      candidates.push_back(std::move(candidate));
    } else if (trace) {
      std::cerr << "stack-aggregate-init-pruning: " << function.name()
                << ": rejected "
                << (initialized ? "overwrite proof" : "initializer shape")
                << '\n';
    }
  }
  if (candidates.empty()) {
    return false;
  }

  std::unordered_map<std::size_t, std::unordered_set<std::size_t>> removals;
  for (const auto &candidate : candidates) {
    for (const auto &location : candidate.initializationStores) {
      removals[location.block].insert(location.instruction);
    }
  }
  for (auto &[blockIndex, indices] : removals) {
    auto &instructions = function.blocks()[blockIndex].instructions();
    std::vector<Instruction> rewritten;
    rewritten.reserve(instructions.size() - indices.size());
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (indices.find(index) == indices.end()) {
        rewritten.push_back(instructions[index]);
      }
    }
    instructions = std::move(rewritten);
  }
  if (std::getenv("COMPILER_STACK_AGGREGATE_INIT_PRUNING_STATS") != nullptr) {
    std::cerr << "stack-aggregate-init-pruning: " << function.name() << ": "
              << candidates.size() << " aggregate(s), " <<
        [&] {
          std::size_t count = 0;
          for (const auto &candidate : candidates) {
            count += candidate.initializationStores.size();
          }
          return count;
        }() << " store(s)\n";
  }
  return true;
}

} // namespace

void StackAggregateInitPruningPass::run(compiler::ir::core::Module &module) {
  for (auto &function : module.functions()) {
    pruneFunction(function);
  }
}

} // namespace compiler::midend::pass
