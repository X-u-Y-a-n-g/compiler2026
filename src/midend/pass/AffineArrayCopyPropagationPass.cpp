#include "../../../include/compiler/midend/pass/AffineArrayCopyPropagationPass.h"

#include <algorithm>
#include <cstddef>
#include <deque>
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

  bool operator==(const Location& other) const {
    return block == other.block && instruction == other.instruction;
  }
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
  std::vector<std::unordered_set<std::size_t>> dominators;
};

struct PointerPath {
  std::string base;
  std::vector<std::string> indices;
};

struct ArrayType {
  std::string scalar;
  std::vector<std::size_t> extents;
};

struct CanonicalLoop {
  std::string induction;
  std::string header;
  std::string preheader;
  std::string latch;
  std::string exit;
  std::size_t headerIndex = 0;
  std::size_t preheaderIndex = 0;
  std::size_t latchIndex = 0;
  std::size_t exitIndex = 0;
  std::unordered_set<std::size_t> blocks;
};

struct LoadRewrite {
  std::string result;
  std::string type;
  std::vector<std::string> sourceIndices;
  Location location;
};

struct Candidate {
  Location store;
  std::string destination;
  std::string source;
  CanonicalLoop outer;
  CanonicalLoop inner;
  std::vector<LoadRewrite> loads;
};

bool isGepOpcode(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "gep";
}

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") {
    return {};
  }
  const auto& operands = terminator->operands();
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

CFG buildCFG(const Function& function) {
  CFG cfg;
  const std::size_t count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.dominators.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < count; ++index) {
    for (const auto& target : branchTargets(function.blocks()[index])) {
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
        for (std::size_t predIndex = 1;
             predIndex < cfg.predecessors[block].size(); ++predIndex) {
          const auto& predDominators =
              cfg.dominators[cfg.predecessors[block][predIndex]];
          for (auto iterator = next.begin(); iterator != next.end();) {
            if (predDominators.find(*iterator) == predDominators.end()) {
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

bool dominates(const CFG& cfg, std::size_t dominator, std::size_t block) {
  return block < cfg.dominators.size() &&
         cfg.dominators[block].find(dominator) != cfg.dominators[block].end();
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto& instructions = function.blocks()[blockIndex].instructions();
    for (std::size_t instructionIndex = 0;
         instructionIndex < instructions.size(); ++instructionIndex) {
      const auto& instruction = instructions[instructionIndex];
      if (instruction.hasResult()) {
        definitions.emplace(
            instruction.result(),
            Definition{&instruction, Location{blockIndex, instructionIndex}});
      }
    }
  }
  return definitions;
}

std::optional<PointerPath> pointerPath(
    const std::string& value, const DefinitionMap& definitions,
    std::unordered_set<std::string>& visiting) {
  if (!value.empty() && value.front() == '@') {
    return PointerPath{value, {}};
  }
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return std::nullopt;
  }
  const auto definition = definitions.find(value);
  if (definition == definitions.end()) {
    return std::nullopt;
  }
  const auto& instruction = *definition->second.instruction;
  if (instruction.opcode() == "alloca") {
    return PointerPath{value, {}};
  }
  if (!isGepOpcode(instruction.opcode()) ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }
  auto path = pointerPath(instruction.operands()[0], definitions, visiting);
  if (!path.has_value()) {
    return std::nullopt;
  }
  path->indices.push_back(instruction.operands()[1]);
  return path;
}

std::optional<PointerPath> pointerPath(const std::string& value,
                                       const DefinitionMap& definitions) {
  std::unordered_set<std::string> visiting;
  return pointerPath(value, definitions, visiting);
}

std::optional<ArrayType> parseArrayType(const std::string& text) {
  const auto firstBracket = text.find('[');
  if (firstBracket == std::string::npos) {
    return std::nullopt;
  }
  ArrayType type;
  type.scalar = text.substr(0, firstBracket);
  std::size_t cursor = firstBracket;
  while (cursor < text.size()) {
    const auto close = text.find(']', cursor + 1);
    if (close == std::string::npos || close == cursor + 1) {
      return std::nullopt;
    }
    long long extent = 0;
    if (!detail::parseIntegerLiteral(
            text.substr(cursor + 1, close - cursor - 1), extent) ||
        extent <= 0) {
      return std::nullopt;
    }
    type.extents.push_back(static_cast<std::size_t>(extent));
    cursor = text.find('[', close + 1);
    if (cursor == std::string::npos) {
      break;
    }
  }
  return type;
}

std::unordered_map<std::string, ArrayType> collectArrayTypes(
    const Module& module) {
  std::unordered_map<std::string, ArrayType> result;
  for (const auto& global : module.globals()) {
    const auto type = parseArrayType(global.type());
    if (type.has_value()) {
      result.emplace("@" + global.name(), *type);
    }
  }
  return result;
}

bool isInteger(const std::string& text, long long expected) {
  long long value = 0;
  return detail::parseIntegerLiteral(text, value) && value == expected;
}

bool branchesTo(const BasicBlock& block, const std::string& target) {
  const auto targets = branchTargets(block);
  return std::find(targets.begin(), targets.end(), target) != targets.end();
}

std::optional<CanonicalLoop> canonicalLoop(
    const Function& function, const CFG& cfg, const DefinitionMap& definitions,
    const std::string& induction, std::size_t expectedBound) {
  const auto phiFound = definitions.find(induction);
  if (phiFound == definitions.end() ||
      phiFound->second.instruction->opcode() != "phi") {
    return std::nullopt;
  }
  const auto& phi = *phiFound->second.instruction;
  const auto& phiOperands = phi.operands();
  if (phiOperands.size() != 5) {
    return std::nullopt;
  }

  std::string preheader;
  std::string latch;
  std::string increment;
  for (std::size_t index = 1; index + 1 < phiOperands.size(); index += 2) {
    if (isInteger(phiOperands[index], 0)) {
      if (!preheader.empty()) {
        return std::nullopt;
      }
      preheader = phiOperands[index + 1];
    } else {
      if (!latch.empty()) {
        return std::nullopt;
      }
      increment = phiOperands[index];
      latch = phiOperands[index + 1];
    }
  }
  if (preheader.empty() || latch.empty()) {
    return std::nullopt;
  }

  const auto headerIndex = phiFound->second.location.block;
  const auto preheaderFound = cfg.index.find(preheader);
  const auto latchFound = cfg.index.find(latch);
  if (preheaderFound == cfg.index.end() || latchFound == cfg.index.end()) {
    return std::nullopt;
  }
  const auto& headerBlock = function.blocks()[headerIndex];
  const auto* headerTerminator = headerBlock.terminator();
  if (headerTerminator == nullptr || headerTerminator->opcode() != "br" ||
      headerTerminator->operands().size() != 3) {
    return std::nullopt;
  }
  const auto conditionFound =
      definitions.find(headerTerminator->operands()[0]);
  if (conditionFound == definitions.end() ||
      conditionFound->second.location.block != headerIndex ||
      conditionFound->second.instruction->opcode() != "icmp.slt" ||
      conditionFound->second.instruction->operands().size() != 2 ||
      conditionFound->second.instruction->operands()[0] != induction ||
      !isInteger(conditionFound->second.instruction->operands()[1],
                 static_cast<long long>(expectedBound))) {
    return std::nullopt;
  }
  const auto exitFound = cfg.index.find(headerTerminator->operands()[2]);
  if (exitFound == cfg.index.end()) {
    return std::nullopt;
  }

  const auto incrementFound = definitions.find(increment);
  if (incrementFound == definitions.end() ||
      incrementFound->second.location.block != latchFound->second ||
      incrementFound->second.instruction->opcode() != "add" ||
      incrementFound->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto& addOperands = incrementFound->second.instruction->operands();
  if (!((addOperands[0] == induction && isInteger(addOperands[1], 1)) ||
        (addOperands[1] == induction && isInteger(addOperands[0], 1)))) {
    return std::nullopt;
  }
  if (!branchesTo(function.blocks()[latchFound->second], headerBlock.name()) ||
      !dominates(cfg, headerIndex, latchFound->second)) {
    return std::nullopt;
  }

  CanonicalLoop loop;
  loop.induction = induction;
  loop.header = headerBlock.name();
  loop.preheader = preheader;
  loop.latch = latch;
  loop.exit = headerTerminator->operands()[2];
  loop.headerIndex = headerIndex;
  loop.preheaderIndex = preheaderFound->second;
  loop.latchIndex = latchFound->second;
  loop.exitIndex = exitFound->second;
  loop.blocks.insert(headerIndex);
  loop.blocks.insert(latchFound->second);
  std::deque<std::size_t> worklist;
  if (latchFound->second != headerIndex) {
    worklist.push_back(latchFound->second);
  }
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto predecessor : cfg.predecessors[block]) {
      if (loop.blocks.insert(predecessor).second && predecessor != headerIndex) {
        worklist.push_back(predecessor);
      }
    }
  }
  if (loop.blocks.find(preheaderFound->second) != loop.blocks.end() ||
      !dominates(cfg, headerIndex, loop.latchIndex)) {
    return std::nullopt;
  }

  std::size_t outsidePredecessors = 0;
  for (const auto predecessor : cfg.predecessors[headerIndex]) {
    if (loop.blocks.find(predecessor) == loop.blocks.end()) {
      ++outsidePredecessors;
      if (predecessor != preheaderFound->second) {
        return std::nullopt;
      }
    }
  }
  if (outsidePredecessors != 1) {
    return std::nullopt;
  }
  const auto* preheaderTerminator =
      function.blocks()[preheaderFound->second].terminator();
  if (preheaderTerminator == nullptr ||
      preheaderTerminator->opcode() != "br" ||
      preheaderTerminator->operands().size() != 1 ||
      preheaderTerminator->operands()[0] != loop.header) {
    return std::nullopt;
  }

  for (const auto block : loop.blocks) {
    for (const auto successor : cfg.successors[block]) {
      if (loop.blocks.find(successor) != loop.blocks.end()) {
        continue;
      }
      if (block != headerIndex || successor != exitFound->second) {
        return std::nullopt;
      }
    }
  }
  return loop;
}

std::optional<std::size_t> constantLoopBound(
    const Function& function, const CFG& cfg, const DefinitionMap& definitions,
    const std::string& induction) {
  const auto phiFound = definitions.find(induction);
  if (phiFound == definitions.end() ||
      phiFound->second.instruction->opcode() != "phi") {
    return std::nullopt;
  }
  const auto* terminator =
      function.blocks()[phiFound->second.location.block].terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3) {
    return std::nullopt;
  }
  const auto condition = definitions.find(terminator->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2 ||
      condition->second.instruction->operands()[0] != induction) {
    return std::nullopt;
  }
  long long bound = 0;
  if (!detail::parseIntegerLiteral(
          condition->second.instruction->operands()[1], bound) ||
      bound <= 0 ||
      static_cast<unsigned long long>(bound) >
          std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  const auto parsed = canonicalLoop(function, cfg, definitions, induction,
                                    static_cast<std::size_t>(bound));
  return parsed.has_value() ? std::optional<std::size_t>{
                                  static_cast<std::size_t>(bound)}
                            : std::nullopt;
}

std::optional<std::size_t> innermostVaryingDimension(
    const Function& function, const CFG& cfg, const DefinitionMap& definitions,
    const PointerPath& access, std::size_t block) {
  std::optional<std::size_t> dimension;
  std::size_t smallestLoop = std::numeric_limits<std::size_t>::max();
  for (std::size_t current = 0; current < access.indices.size(); ++current) {
    const auto bound =
        constantLoopBound(function, cfg, definitions, access.indices[current]);
    if (!bound.has_value()) {
      continue;
    }
    const auto loop = canonicalLoop(function, cfg, definitions,
                                    access.indices[current], *bound);
    if (!loop.has_value() || loop->blocks.find(block) == loop->blocks.end() ||
        loop->blocks.size() >= smallestLoop) {
      continue;
    }
    smallestLoop = loop->blocks.size();
    dimension = current;
  }
  return dimension;
}

long double rowMajorStride(const ArrayType& type, std::size_t dimension) {
  long double stride = 1.0L;
  for (std::size_t current = dimension + 1; current < type.extents.size();
       ++current) {
    stride *= static_cast<long double>(type.extents[current]);
  }
  return stride;
}

std::unordered_set<std::size_t> reachableFrom(
    const CFG& cfg, std::size_t start, bool reverse) {
  std::unordered_set<std::size_t> reachable;
  std::deque<std::size_t> worklist;
  reachable.insert(start);
  worklist.push_back(start);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    const auto& edges = reverse ? cfg.predecessors[block] : cfg.successors[block];
    for (const auto next : edges) {
      if (reachable.insert(next).second) {
        worklist.push_back(next);
      }
    }
  }
  return reachable;
}

bool destinationUsesAreConfined(
    const Module& module, std::size_t functionIndex,
    const std::string& destination, const Location& candidateStore,
    std::vector<Location>& loads) {
  for (std::size_t currentFunction = 0;
       currentFunction < module.functions().size(); ++currentFunction) {
    const auto& function = module.functions()[currentFunction];
    const auto definitions = buildDefinitions(function);
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      const auto& instructions = function.blocks()[blockIndex].instructions();
      for (std::size_t instructionIndex = 0;
           instructionIndex < instructions.size(); ++instructionIndex) {
        const auto& instruction = instructions[instructionIndex];
        for (std::size_t operandIndex = 0;
             operandIndex < instruction.operands().size(); ++operandIndex) {
          const auto& operand = instruction.operands()[operandIndex];
          bool destinationPointer = operand == destination;
          if (!destinationPointer && !operand.empty() && operand.front() == '%') {
            const auto path = pointerPath(operand, definitions);
            destinationPointer = path.has_value() && path->base == destination;
          }
          if (!destinationPointer) {
            continue;
          }
          if (currentFunction != functionIndex) {
            return false;
          }
          if (isGepOpcode(instruction.opcode()) && operandIndex == 0) {
            continue;
          }
          if (instruction.opcode() == "load" && operandIndex == 1) {
            loads.push_back(Location{blockIndex, instructionIndex});
            continue;
          }
          if (instruction.opcode() == "store" && operandIndex == 2 &&
              Location{blockIndex, instructionIndex} == candidateStore) {
            continue;
          }
          return false;
        }
      }
    }
  }
  std::sort(loads.begin(), loads.end(), [](const Location& lhs, const Location& rhs) {
    return lhs.block < rhs.block ||
           (lhs.block == rhs.block && lhs.instruction < rhs.instruction);
  });
  loads.erase(std::unique(loads.begin(), loads.end()), loads.end());
  return !loads.empty();
}

bool pathMayModifySource(const Function& function, const CFG& cfg,
                         const DefinitionMap& definitions,
                         const CanonicalLoop& producer,
                         const std::string& source, const Location& load) {
  const auto forward = reachableFrom(cfg, producer.exitIndex, false);
  if (forward.find(load.block) == forward.end()) {
    return true;
  }
  const auto backward = reachableFrom(cfg, load.block, true);
  for (const auto block : forward) {
    if (backward.find(block) == backward.end()) {
      continue;
    }
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.opcode() == "call") {
        return true;
      }
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3) {
        continue;
      }
      const auto path = pointerPath(instruction.operands()[2], definitions);
      if (!path.has_value()) {
        return true;
      }
      if (path->base == source) {
        return true;
      }
      if (!path->base.empty() && path->base.front() == '@') {
        continue;
      }
      const auto baseDefinition = definitions.find(path->base);
      if (baseDefinition == definitions.end() ||
          baseDefinition->second.instruction->opcode() != "alloca") {
        return true;
      }
    }
  }
  return false;
}

bool loopCanBeRemoved(const Function& function,
                      const CanonicalLoop& loop, const Location& copyStore) {
  std::unordered_set<std::string> loopDefinitions;
  for (const auto block : loop.blocks) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto& instruction = instructions[index];
      if (instruction.hasResult()) {
        loopDefinitions.insert(instruction.result());
      }
      if (instruction.opcode() == "call" || instruction.opcode() == "ret") {
        return false;
      }
      if (instruction.opcode() == "store" &&
          !(Location{block, index} == copyStore)) {
        return false;
      }
    }
  }

  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (loop.blocks.find(block) != loop.blocks.end()) {
      continue;
    }
    for (const auto& instruction : function.blocks()[block].instructions()) {
      const auto& operands = instruction.operands();
      if (instruction.opcode() == "phi") {
        for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
          if (loopDefinitions.find(operands[index]) != loopDefinitions.end()) {
            return false;
          }
        }
        continue;
      }
      for (const auto& operand : operands) {
        if (loopDefinitions.find(operand) != loopDefinitions.end()) {
          return false;
        }
      }
    }
  }

  const auto& exitBlock = function.blocks()[loop.exitIndex];
  for (const auto& instruction : exitBlock.instructions()) {
    if (instruction.opcode() != "phi") {
      break;
    }
    const auto& operands = instruction.operands();
    for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
      if (operands[index + 1] == loop.header &&
          loopDefinitions.find(operands[index]) != loopDefinitions.end()) {
        return false;
      }
    }
  }
  return true;
}

std::optional<Candidate> analyzeStore(
    const Module& module, std::size_t functionIndex, const CFG& cfg,
    const DefinitionMap& definitions,
    const std::unordered_map<std::string, ArrayType>& arrayTypes,
    const Location& storeLocation) {
  const auto& function = module.functions()[functionIndex];
  const auto& store = function.blocks()[storeLocation.block]
                          .instructions()[storeLocation.instruction];
  if (store.opcode() != "store" || store.operands().size() != 3) {
    return std::nullopt;
  }
  const auto destinationPath = pointerPath(store.operands()[2], definitions);
  const auto valueDefinition = definitions.find(store.operands()[1]);
  if (!destinationPath.has_value() || destinationPath->indices.size() != 2 ||
      valueDefinition == definitions.end() ||
      valueDefinition->second.instruction->opcode() != "load" ||
      valueDefinition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto sourcePath = pointerPath(
      valueDefinition->second.instruction->operands()[1], definitions);
  if (!sourcePath.has_value() || sourcePath->indices.size() != 2 ||
      sourcePath->base == destinationPath->base) {
    return std::nullopt;
  }
  const auto destinationType = arrayTypes.find(destinationPath->base);
  const auto sourceType = arrayTypes.find(sourcePath->base);
  if (destinationType == arrayTypes.end() || sourceType == arrayTypes.end() ||
      destinationType->second.extents.size() != 2 ||
      sourceType->second.extents.size() != 2 ||
      destinationType->second.scalar != sourceType->second.scalar ||
      destinationPath->indices[0] == destinationPath->indices[1]) {
    return std::nullopt;
  }
  for (const auto extent : destinationType->second.extents) {
    if (extent > static_cast<std::size_t>(
                     std::numeric_limits<int>::max())) {
      return std::nullopt;
    }
  }

  std::vector<std::size_t> sourceFromDestination(2, 2);
  for (std::size_t sourceDimension = 0; sourceDimension < 2;
       ++sourceDimension) {
    for (std::size_t destinationDimension = 0; destinationDimension < 2;
         ++destinationDimension) {
      if (sourcePath->indices[sourceDimension] ==
          destinationPath->indices[destinationDimension]) {
        sourceFromDestination[sourceDimension] = destinationDimension;
      }
    }
    if (sourceFromDestination[sourceDimension] == 2 ||
        sourceType->second.extents[sourceDimension] !=
            destinationType->second.extents[
                sourceFromDestination[sourceDimension]]) {
      return std::nullopt;
    }
  }
  if (sourceFromDestination[0] == sourceFromDestination[1]) {
    return std::nullopt;
  }

  const auto loop0 = canonicalLoop(
      function, cfg, definitions, destinationPath->indices[0],
      destinationType->second.extents[0]);
  const auto loop1 = canonicalLoop(
      function, cfg, definitions, destinationPath->indices[1],
      destinationType->second.extents[1]);
  if (!loop0.has_value() || !loop1.has_value()) {
    return std::nullopt;
  }

  CanonicalLoop outer;
  CanonicalLoop inner;
  if (loop0->blocks.find(loop1->headerIndex) != loop0->blocks.end() &&
      loop1->blocks.find(loop0->headerIndex) == loop1->blocks.end()) {
    outer = *loop0;
    inner = *loop1;
  } else if (loop1->blocks.find(loop0->headerIndex) != loop1->blocks.end() &&
             loop0->blocks.find(loop1->headerIndex) == loop0->blocks.end()) {
    outer = *loop1;
    inner = *loop0;
  } else {
    return std::nullopt;
  }
  if (outer.blocks.find(storeLocation.block) == outer.blocks.end() ||
      inner.blocks.find(storeLocation.block) == inner.blocks.end() ||
      outer.blocks.find(inner.preheaderIndex) == outer.blocks.end() ||
      !dominates(cfg, storeLocation.block, inner.latchIndex)) {
    return std::nullopt;
  }
  if (!loopCanBeRemoved(function, outer, storeLocation)) {
    return std::nullopt;
  }

  std::vector<Location> loadLocations;
  if (!destinationUsesAreConfined(module, functionIndex,
                                  destinationPath->base, storeLocation,
                                  loadLocations)) {
    return std::nullopt;
  }

  Candidate candidate;
  candidate.store = storeLocation;
  candidate.destination = destinationPath->base;
  candidate.source = sourcePath->base;
  candidate.outer = std::move(outer);
  candidate.inner = std::move(inner);
  long double localityDelta = 0.0L;
  for (const auto& loadLocation : loadLocations) {
    if (candidate.outer.blocks.find(loadLocation.block) !=
            candidate.outer.blocks.end() ||
        !dominates(cfg, candidate.outer.exitIndex, loadLocation.block) ||
        pathMayModifySource(function, cfg, definitions, candidate.outer,
                            candidate.source, loadLocation)) {
      return std::nullopt;
    }
    const auto& load = function.blocks()[loadLocation.block]
                           .instructions()[loadLocation.instruction];
    if (load.opcode() != "load" || !load.hasResult() ||
        load.operands().size() != 2) {
      return std::nullopt;
    }
    const auto loadPath = pointerPath(load.operands()[1], definitions);
    if (!loadPath.has_value() || loadPath->base != candidate.destination ||
        loadPath->indices.size() != 2) {
      return std::nullopt;
    }
    const auto varyingDimension = innermostVaryingDimension(
        function, cfg, definitions, *loadPath, loadLocation.block);
    if (varyingDimension.has_value()) {
      std::size_t sourceDimension = 2;
      for (std::size_t current = 0; current < 2; ++current) {
        if (sourceFromDestination[current] == *varyingDimension) {
          sourceDimension = current;
          break;
        }
      }
      if (sourceDimension == 2) {
        return std::nullopt;
      }
      localityDelta += rowMajorStride(sourceType->second, sourceDimension) -
                       rowMajorStride(destinationType->second,
                                      *varyingDimension);
    }
    std::vector<std::string> newIndices(2);
    for (std::size_t sourceDimension = 0; sourceDimension < 2;
         ++sourceDimension) {
      newIndices[sourceDimension] =
          loadPath->indices[sourceFromDestination[sourceDimension]];
    }
    candidate.loads.push_back(LoadRewrite{load.result(), load.operands()[0],
                                          std::move(newIndices), loadLocation});
  }
  // A full copy is worth removing only when forwarding does not make the
  // statically observed innermost-loop access strides worse in aggregate.
  // This conservatively preserves deliberately materialized layouts that are
  // reused primarily through their contiguous dimension.
  if (localityDelta > 0.0L) {
    return std::nullopt;
  }
  return candidate;
}

std::string freshValueName(std::unordered_set<std::string>& names,
                           std::size_t& counter) {
  while (true) {
    const std::string candidate = "%affine.copy.gep." +
                                  std::to_string(counter++);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

void applyCandidate(Function& function, const Candidate& candidate) {
  std::unordered_map<std::string, const LoadRewrite*> rewrites;
  for (const auto& load : candidate.loads) {
    rewrites.emplace(load.result, &load);
  }
  std::unordered_set<std::string> valueNames;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        valueNames.insert(instruction.result());
      }
    }
  }
  std::size_t nameCounter = 0;
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size() + candidate.loads.size() * 2);
    for (const auto& instruction : block.instructions()) {
      const auto found = rewrites.find(instruction.result());
      if (instruction.opcode() != "load" || found == rewrites.end()) {
        rewritten.push_back(instruction);
        continue;
      }
      const auto* load = found->second;
      const auto row = freshValueName(valueNames, nameCounter);
      const auto element = freshValueName(valueNames, nameCounter);
      rewritten.emplace_back("getelementptr",
                             std::vector<std::string>{candidate.source,
                                                      load->sourceIndices[0]},
                             row);
      rewritten.emplace_back("getelementptr",
                             std::vector<std::string>{row,
                                                      load->sourceIndices[1]},
                             element);
      rewritten.emplace_back("load",
                             std::vector<std::string>{load->type, element},
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  auto& preheaderInstructions =
      function.blocks()[candidate.outer.preheaderIndex].instructions();
  preheaderInstructions.back() = Instruction{"br", {candidate.outer.exit}};

  auto& exitInstructions =
      function.blocks()[candidate.outer.exitIndex].instructions();
  for (auto& instruction : exitInstructions) {
    if (instruction.opcode() != "phi") {
      break;
    }
    auto operands = instruction.operands();
    for (std::size_t index = 2; index < operands.size(); index += 2) {
      if (operands[index] == candidate.outer.header) {
        operands[index] = candidate.outer.preheader;
      }
    }
    instruction = Instruction{"phi", std::move(operands), instruction.result()};
  }
}

bool rewriteOne(Module& module, std::size_t functionIndex) {
  const auto arrayTypes = collectArrayTypes(module);
  const auto& function = module.functions()[functionIndex];
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (std::size_t instruction = 0;
         instruction < function.blocks()[block].instructions().size();
         ++instruction) {
      const auto candidate =
          analyzeStore(module, functionIndex, cfg, definitions, arrayTypes,
                       Location{block, instruction});
      if (!candidate.has_value()) {
        continue;
      }
      applyCandidate(module.functions()[functionIndex], *candidate);
      return true;
    }
  }
  return false;
}

}  // namespace

void AffineArrayCopyPropagationPass::run(Module& module) {
  for (std::size_t function = 0; function < module.functions().size();
       ++function) {
    while (rewriteOne(module, function)) {
    }
  }
}

}  // namespace compiler::midend::pass
