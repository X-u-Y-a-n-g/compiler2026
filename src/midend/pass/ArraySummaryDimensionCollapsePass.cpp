#include "../../../include/compiler/midend/pass/ArraySummaryDimensionCollapsePass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"
#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Constant;
using compiler::ir::core::Function;
using compiler::ir::core::GlobalVariable;
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

struct PointerPath {
  std::string base;
  std::vector<std::string> indices;
};

struct ArrayType {
  std::string scalar;
  std::size_t rows = 0;
  std::size_t columns = 0;
};

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<std::unordered_set<std::size_t>> dominators;
};

struct CanonicalLoop {
  std::string induction;
  std::string bound;
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

struct TransformPattern {
  std::size_t functionIndex = 0;
  std::string count;
  std::string factor;
  std::string source;
  std::string destination;
  std::string row;
  std::string stage;
  std::string column;
  bool skipsUnitFactor = false;
  std::size_t columns = 0;
};

struct ReductionPattern {
  std::string matrix;
  std::string accumulator;
  Location matrixLoad;
  bool accumulatorInMemory = true;
  CanonicalLoop outer;
  CanonicalLoop inner;
};

struct CallSite {
  Location location;
  std::string count;
  std::string factor;
  std::string source;
  std::string destination;
};

struct CollapsePlan {
  std::size_t callerIndex = 0;
  TransformPattern transform;
  CanonicalLoop callLoop;
  std::vector<CallSite> calls;
  std::unordered_map<std::string, ArrayType> matrices;
  std::unordered_map<std::string, ReductionPattern> reductions;
  bool precomputeCoefficients = false;
};

bool isGep(const std::string &opcode) {
  return opcode == "getelementptr" || opcode == "gep";
}

bool isInteger(const std::string &value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

DefinitionMap buildDefinitions(const Function &function) {
  DefinitionMap result;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto &instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].hasResult()) {
        result.emplace(instructions[index].result(),
                       Definition{&instructions[index], {block, index}});
      }
    }
  }
  return result;
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

bool reachable(const CFG &cfg, std::size_t start, std::size_t target) {
  std::vector<bool> visited(cfg.successors.size(), false);
  std::deque<std::size_t> worklist;
  worklist.push_back(start);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    if (block == target) {
      return true;
    }
    if (block >= visited.size() || visited[block]) {
      continue;
    }
    visited[block] = true;
    for (const auto successor : cfg.successors[block]) {
      worklist.push_back(successor);
    }
  }
  return false;
}

bool reachableAvoiding(const CFG &cfg, std::size_t start, std::size_t target,
                       std::size_t forbidden) {
  std::vector<bool> visited(cfg.successors.size(), false);
  std::deque<std::size_t> worklist;
  worklist.push_back(start);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    if (block == forbidden) {
      continue;
    }
    if (block == target) {
      return true;
    }
    if (block >= visited.size() || visited[block]) {
      continue;
    }
    visited[block] = true;
    for (const auto successor : cfg.successors[block]) {
      worklist.push_back(successor);
    }
  }
  return false;
}

std::optional<PointerPath>
pointerPath(const std::string &value, const DefinitionMap &definitions,
            std::unordered_set<std::string> &visiting) {
  if (!value.empty() && value.front() == '@') {
    return PointerPath{value, {}};
  }
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return std::nullopt;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end()) {
    return PointerPath{value, {}};
  }
  const auto &instruction = *found->second.instruction;
  if (instruction.opcode() == "alloca") {
    return PointerPath{value, {}};
  }
  if (!isGep(instruction.opcode()) || instruction.operands().size() != 2) {
    return std::nullopt;
  }
  auto result = pointerPath(instruction.operands()[0], definitions, visiting);
  if (!result.has_value()) {
    return std::nullopt;
  }
  result->indices.push_back(instruction.operands()[1]);
  return result;
}

std::optional<PointerPath> pointerPath(const std::string &value,
                                       const DefinitionMap &definitions) {
  std::unordered_set<std::string> visiting;
  return pointerPath(value, definitions, visiting);
}

std::optional<ArrayType> parseTwoDimensionalArray(const std::string &text) {
  const auto firstOpen = text.find('[');
  const auto firstClose = firstOpen == std::string::npos
                              ? std::string::npos
                              : text.find(']', firstOpen + 1);
  const auto secondOpen = firstClose == std::string::npos
                              ? std::string::npos
                              : text.find('[', firstClose + 1);
  const auto secondClose = secondOpen == std::string::npos
                               ? std::string::npos
                               : text.find(']', secondOpen + 1);
  if (firstOpen == std::string::npos || firstClose == std::string::npos ||
      secondOpen == std::string::npos || secondClose == std::string::npos ||
      text.find('[', secondClose + 1) != std::string::npos) {
    return std::nullopt;
  }
  long long rows = 0;
  long long columns = 0;
  const auto first = text.substr(firstOpen + 1, firstClose - firstOpen - 1);
  const auto second = text.substr(secondOpen + 1, secondClose - secondOpen - 1);
  if (first.empty()) {
    rows = 0;
  } else if (!detail::parseIntegerLiteral(first, rows) || rows <= 0) {
    return std::nullopt;
  }
  if (!detail::parseIntegerLiteral(second, columns) || columns <= 0) {
    return std::nullopt;
  }
  return ArrayType{text.substr(0, firstOpen), static_cast<std::size_t>(rows),
                   static_cast<std::size_t>(columns)};
}

std::unordered_map<std::string, ArrayType>
collectGlobalArrays(const Module &module) {
  std::unordered_map<std::string, ArrayType> result;
  for (const auto &global : module.globals()) {
    const auto type = parseTwoDimensionalArray(global.type());
    if (type.has_value() && type->scalar == "int") {
      result.emplace("@" + global.name(), *type);
    }
  }
  return result;
}

std::optional<CanonicalLoop> canonicalLoop(const Function &function,
                                           const CFG &cfg,
                                           const DefinitionMap &definitions,
                                           const std::string &induction,
                                           const std::string &expectedBound) {
  const auto phiFound = definitions.find(induction);
  if (phiFound == definitions.end() ||
      phiFound->second.instruction->opcode() != "phi") {
    return std::nullopt;
  }
  const auto &phi = *phiFound->second.instruction;
  if (phi.operands().size() < 5 || phi.operands().size() % 2 == 0) {
    return std::nullopt;
  }
  std::string preheader;
  std::vector<std::pair<std::string, std::string>> backedges;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (isInteger(phi.operands()[index], 0)) {
      if (!preheader.empty()) {
        return std::nullopt;
      }
      preheader = phi.operands()[index + 1];
    } else {
      backedges.emplace_back(phi.operands()[index], phi.operands()[index + 1]);
    }
  }
  const auto preheaderFound = cfg.index.find(preheader);
  if (preheader.empty() || backedges.empty() ||
      preheaderFound == cfg.index.end()) {
    return std::nullopt;
  }
  const auto headerIndex = phiFound->second.location.block;
  const auto *terminator = function.blocks()[headerIndex].terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3) {
    return std::nullopt;
  }
  const auto condition = definitions.find(terminator->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.location.block != headerIndex ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2 ||
      condition->second.instruction->operands()[0] != induction ||
      condition->second.instruction->operands()[1] != expectedBound) {
    return std::nullopt;
  }
  const auto exitFound = cfg.index.find(terminator->operands()[2]);
  if (exitFound == cfg.index.end()) {
    return std::nullopt;
  }
  std::vector<std::size_t> latchIndices;
  for (const auto &[increment, latch] : backedges) {
    const auto latchFound = cfg.index.find(latch);
    const auto incrementFound = definitions.find(increment);
    if (latchFound == cfg.index.end() || incrementFound == definitions.end() ||
        incrementFound->second.location.block != latchFound->second ||
        incrementFound->second.instruction->opcode() != "add" ||
        incrementFound->second.instruction->operands().size() != 2 ||
        !dominates(cfg, headerIndex, latchFound->second) ||
        std::find(cfg.successors[latchFound->second].begin(),
                  cfg.successors[latchFound->second].end(),
                  headerIndex) == cfg.successors[latchFound->second].end()) {
      return std::nullopt;
    }
    const auto &add = incrementFound->second.instruction->operands();
    if (!((add[0] == induction && isInteger(add[1], 1)) ||
          (add[1] == induction && isInteger(add[0], 1)))) {
      return std::nullopt;
    }
    latchIndices.push_back(latchFound->second);
  }
  CanonicalLoop loop;
  loop.induction = induction;
  loop.bound = expectedBound;
  loop.header = function.blocks()[headerIndex].name();
  loop.preheader = preheader;
  loop.latch = backedges.front().second;
  loop.exit = terminator->operands()[2];
  loop.headerIndex = headerIndex;
  loop.preheaderIndex = preheaderFound->second;
  loop.latchIndex = latchIndices.front();
  loop.exitIndex = exitFound->second;
  loop.blocks.insert(headerIndex);
  for (const auto latch : latchIndices) {
    loop.blocks.insert(latch);
  }
  std::deque<std::size_t> worklist;
  for (const auto latch : latchIndices) {
    worklist.push_back(latch);
  }
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto predecessor : cfg.predecessors[block]) {
      if (loop.blocks.insert(predecessor).second &&
          predecessor != headerIndex) {
        worklist.push_back(predecessor);
      }
    }
  }
  if (loop.blocks.find(preheaderFound->second) != loop.blocks.end()) {
    return std::nullopt;
  }
  std::size_t outsidePredecessors = 0;
  for (const auto predecessor : cfg.predecessors[headerIndex]) {
    if (loop.blocks.find(predecessor) == loop.blocks.end()) {
      if (predecessor != preheaderFound->second) {
        return std::nullopt;
      }
      ++outsidePredecessors;
    }
  }
  if (outsidePredecessors != 1) {
    return std::nullopt;
  }
  for (const auto block : loop.blocks) {
    for (const auto successor : cfg.successors[block]) {
      if (loop.blocks.find(successor) == loop.blocks.end() &&
          !(block == headerIndex && successor == exitFound->second)) {
        return std::nullopt;
      }
    }
  }
  return loop;
}

std::optional<const Instruction *>
definingInstruction(const std::string &value, const DefinitionMap &definitions,
                    const std::string &opcode) {
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != opcode) {
    return std::nullopt;
  }
  return found->second.instruction;
}

bool samePath(const PointerPath &first, const PointerPath &second) {
  return first.base == second.base && first.indices == second.indices;
}

std::optional<TransformPattern> analyzeTransform(const Module &module,
                                                 std::size_t functionIndex) {
  const auto &function = module.functions()[functionIndex];
  if (function.returnType() != "void") {
    return std::nullopt;
  }
  const auto definitions = buildDefinitions(function);
  const auto cfg = buildCFG(function);
  std::optional<TransformPattern> result;
  Location updateStore;
  Location zeroStore;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto &instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto &store = instructions[index];
      if (store.opcode() != "store" || store.operands().size() != 3 ||
          store.operands()[0] != "int") {
        continue;
      }
      const auto destinationPath =
          pointerPath(store.operands()[2], definitions);
      if (!destinationPath.has_value() ||
          destinationPath->indices.size() != 2) {
        continue;
      }
      if (isInteger(store.operands()[1], 0)) {
        zeroStore = {block, index};
        continue;
      }
      const auto add =
          definingInstruction(store.operands()[1], definitions, "add");
      if (!add.has_value() || (*add)->operands().size() != 2) {
        continue;
      }
      const Instruction *multiply = nullptr;
      const Instruction *sourceLoad = nullptr;
      for (std::size_t operand = 0; operand < 2; ++operand) {
        const auto maybeMultiply = definingInstruction(
            (*add)->operands()[operand], definitions, "mul");
        const auto maybeLoad = definingInstruction(
            (*add)->operands()[1 - operand], definitions, "load");
        if (maybeMultiply.has_value() && maybeLoad.has_value()) {
          multiply = *maybeMultiply;
          sourceLoad = *maybeLoad;
          break;
        }
      }
      if (multiply == nullptr || sourceLoad == nullptr ||
          multiply->operands().size() != 2 ||
          sourceLoad->operands().size() != 2) {
        continue;
      }
      const Instruction *destinationLoad = nullptr;
      const Instruction *factorLoad = nullptr;
      for (std::size_t operand = 0; operand < 2; ++operand) {
        const auto first = definingInstruction(multiply->operands()[operand],
                                               definitions, "load");
        const auto second = definingInstruction(
            multiply->operands()[1 - operand], definitions, "load");
        if (!first.has_value() || !second.has_value()) {
          continue;
        }
        const auto firstPath =
            pointerPath((*first)->operands()[1], definitions);
        const auto secondPath =
            pointerPath((*second)->operands()[1], definitions);
        if (firstPath.has_value() && secondPath.has_value() &&
            samePath(*firstPath, *destinationPath)) {
          destinationLoad = *first;
          factorLoad = *second;
          break;
        }
      }
      if (destinationLoad == nullptr || factorLoad == nullptr) {
        continue;
      }
      const auto sourcePath =
          pointerPath(sourceLoad->operands()[1], definitions);
      const auto factorPath =
          pointerPath(factorLoad->operands()[1], definitions);
      if (!sourcePath.has_value() || !factorPath.has_value() ||
          sourcePath->indices.size() != 2 || factorPath->indices.size() != 2) {
        continue;
      }
      const auto row = destinationPath->indices[0];
      const auto column = destinationPath->indices[1];
      const auto stage = sourcePath->indices[0];
      if (sourcePath->indices[1] != column || factorPath->indices[0] != row ||
          factorPath->indices[1] != stage ||
          destinationPath->base == sourcePath->base ||
          destinationPath->base == factorPath->base ||
          sourcePath->base == factorPath->base) {
        continue;
      }
      if (result.has_value()) {
        return std::nullopt;
      }
      result = TransformPattern{functionIndex,
                                "",
                                factorPath->base,
                                sourcePath->base,
                                destinationPath->base,
                                row,
                                stage,
                                column,
                                false,
                                0};
      updateStore = {block, index};
    }
  }
  if (!result.has_value() || zeroStore.block >= function.blocks().size()) {
    return std::nullopt;
  }
  const auto parameterType =
      [&](const std::string &name) -> std::optional<ArrayType> {
    for (const auto &parameter : function.parameters()) {
      if (parameter.name == name) {
        return parseTwoDimensionalArray(parameter.type);
      }
    }
    return std::nullopt;
  };
  const auto factorType = parameterType(result->factor);
  const auto sourceType = parameterType(result->source);
  const auto destinationType = parameterType(result->destination);
  if (!factorType.has_value() || !sourceType.has_value() ||
      !destinationType.has_value() || factorType->scalar != "int" ||
      sourceType->scalar != "int" || destinationType->scalar != "int" ||
      factorType->columns != sourceType->columns ||
      factorType->columns != destinationType->columns) {
    return std::nullopt;
  }
  result->columns = factorType->columns;
  for (const auto &parameter : function.parameters()) {
    if (parameter.type == "int" && parameter.name != result->row &&
        parameter.name != result->stage && parameter.name != result->column) {
      result->count = parameter.name;
      break;
    }
  }
  if (result->count.empty()) {
    return std::nullopt;
  }
  const auto rowLoop =
      canonicalLoop(function, cfg, definitions, result->row, result->count);
  const auto stageLoop =
      canonicalLoop(function, cfg, definitions, result->stage, result->count);
  const auto columnLoop =
      canonicalLoop(function, cfg, definitions, result->column, result->count);
  if (!rowLoop.has_value() || !stageLoop.has_value() ||
      !columnLoop.has_value() ||
      rowLoop->blocks.find(updateStore.block) == rowLoop->blocks.end() ||
      stageLoop->blocks.find(updateStore.block) == stageLoop->blocks.end() ||
      columnLoop->blocks.find(updateStore.block) == columnLoop->blocks.end() ||
      columnLoop->blocks.size() >= rowLoop->blocks.size() ||
      columnLoop->blocks.size() >= stageLoop->blocks.size()) {
    return std::nullopt;
  }
  const auto &zero =
      function.blocks()[zeroStore.block].instructions()[zeroStore.instruction];
  const auto zeroPath = pointerPath(zero.operands()[2], definitions);
  if (!zeroPath.has_value() || zeroPath->base != result->destination ||
      zeroPath->indices.size() != 2) {
    return std::nullopt;
  }
  const auto zeroRow = canonicalLoop(function, cfg, definitions,
                                     zeroPath->indices[0], result->count);
  const auto zeroColumn = canonicalLoop(function, cfg, definitions,
                                        zeroPath->indices[1], result->count);
  if (!zeroRow.has_value() || !zeroColumn.has_value() ||
      zeroRow->blocks.find(zeroStore.block) == zeroRow->blocks.end() ||
      zeroColumn->blocks.find(zeroStore.block) == zeroColumn->blocks.end()) {
    return std::nullopt;
  }
  std::size_t stores = 0;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "call") {
        return std::nullopt;
      }
      if (instruction.opcode() != "store") {
        continue;
      }
      ++stores;
      const auto path = pointerPath(instruction.operands()[2], definitions);
      if (!path.has_value() || path->base != result->destination ||
          path->indices.size() != 2) {
        return std::nullopt;
      }
    }
  }
  if (stores != 2) {
    return std::nullopt;
  }
  std::size_t skipComparisons = 0;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto &block = function.blocks()[blockIndex];
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "icmp.eq" ||
          instruction.operands().size() != 2) {
        continue;
      }
      std::string compared;
      if (isInteger(instruction.operands()[0], 1)) {
        compared = instruction.operands()[1];
      } else if (isInteger(instruction.operands()[1], 1)) {
        compared = instruction.operands()[0];
      }
      const auto load = definingInstruction(compared, definitions, "load");
      if (!load.has_value()) {
        continue;
      }
      const auto path = pointerPath((*load)->operands()[1], definitions);
      if (path.has_value() && path->base == result->factor &&
          path->indices ==
              std::vector<std::string>{result->row, result->stage}) {
        const auto *terminator = block.terminator();
        if (terminator == nullptr || terminator->opcode() != "br" ||
            terminator->operands().size() != 3 ||
            terminator->operands()[0] != instruction.result()) {
          return std::nullopt;
        }
        const auto trueFound = cfg.index.find(terminator->operands()[1]);
        const auto falseFound = cfg.index.find(terminator->operands()[2]);
        if (trueFound == cfg.index.end() || falseFound == cfg.index.end() ||
            falseFound->second != columnLoop->preheaderIndex ||
            !reachableAvoiding(cfg, trueFound->second, rowLoop->headerIndex,
                               updateStore.block) ||
            reachableAvoiding(cfg, trueFound->second, updateStore.block,
                              rowLoop->headerIndex) ||
            !reachableAvoiding(cfg, falseFound->second, updateStore.block,
                               rowLoop->headerIndex)) {
          return std::nullopt;
        }
        ++skipComparisons;
      }
    }
  }
  if (skipComparisons > 1) {
    return std::nullopt;
  }
  result->skipsUnitFactor = skipComparisons == 1;
  for (const auto blockIndex : stageLoop->blocks) {
    const auto *terminator = function.blocks()[blockIndex].terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      if (!result->skipsUnitFactor && terminator != nullptr &&
          terminator->opcode() == "br" &&
          terminator->operands().size() == 1) {
        continue;
      }
      continue;
    }
    if (blockIndex != stageLoop->headerIndex &&
        blockIndex != rowLoop->headerIndex &&
        blockIndex != columnLoop->headerIndex) {
      if (!result->skipsUnitFactor) {
        return std::nullopt;
      }
      const auto condition = definitions.find(terminator->operands()[0]);
      if (condition == definitions.end() ||
          condition->second.instruction->opcode() != "icmp.eq") {
        return std::nullopt;
      }
    }
  }
  return result;
}

std::optional<ReductionPattern>
analyzeReduction(const Function &function, const CFG &cfg,
                 const DefinitionMap &definitions,
                 const Location &storeLocation, const std::string &count) {
  const auto &store = function.blocks()[storeLocation.block]
                          .instructions()[storeLocation.instruction];
  const Instruction *addInstruction = nullptr;
  std::optional<PointerPath> accumulatorPath;
  bool accumulatorInMemory = true;
  if (store.opcode() == "store" && store.operands().size() == 3) {
    accumulatorPath = pointerPath(store.operands()[2], definitions);
    const auto add =
        definingInstruction(store.operands()[1], definitions, "add");
    if (!accumulatorPath.has_value() || !accumulatorPath->indices.empty() ||
        !add.has_value() || (*add)->operands().size() != 2) {
      return std::nullopt;
    }
    addInstruction = *add;
  } else if (store.opcode() == "add" && store.hasResult() &&
             store.operands().size() == 2) {
    addInstruction = &store;
    accumulatorInMemory = false;
  } else {
    return std::nullopt;
  }
  const Instruction *accumulatorLoad = nullptr;
  const Instruction *matrixLoad = nullptr;
  for (std::size_t operand = 0; operand < 2; ++operand) {
    const auto matrixCandidate = definingInstruction(
        addInstruction->operands()[operand], definitions, "load");
    if (!matrixCandidate.has_value()) {
      continue;
    }
    const auto matrixPath =
        pointerPath((*matrixCandidate)->operands()[1], definitions);
    if (!matrixPath.has_value() || matrixPath->base.empty() ||
        matrixPath->base.front() != '@' || matrixPath->indices.size() != 2) {
      continue;
    }
    if (accumulatorInMemory) {
      const auto other = definingInstruction(
          addInstruction->operands()[1 - operand], definitions, "load");
      if (!other.has_value()) {
        continue;
      }
      const auto otherPath = pointerPath((*other)->operands()[1], definitions);
      if (!otherPath.has_value() || otherPath->base != accumulatorPath->base ||
          !otherPath->indices.empty()) {
        continue;
      }
      accumulatorLoad = *other;
    }
    matrixLoad = *matrixCandidate;
    break;
  }
  if ((accumulatorInMemory && accumulatorLoad == nullptr) ||
      matrixLoad == nullptr) {
    return std::nullopt;
  }
  const auto matrixPath = pointerPath(matrixLoad->operands()[1], definitions);
  const auto outer =
      canonicalLoop(function, cfg, definitions, matrixPath->indices[0], count);
  const auto inner =
      canonicalLoop(function, cfg, definitions, matrixPath->indices[1], count);
  if (!outer.has_value() || !inner.has_value() ||
      outer->blocks.find(inner->headerIndex) == outer->blocks.end() ||
      inner->blocks.find(storeLocation.block) == inner->blocks.end() ||
      inner->blocks.size() >= outer->blocks.size()) {
    return std::nullopt;
  }
  std::string accumulator =
      accumulatorInMemory ? accumulatorPath->base
      : addInstruction->operands()[0] == matrixLoad->result()
          ? addInstruction->operands()[1]
          : addInstruction->operands()[0];
  if (!accumulatorInMemory) {
    const auto innerPhi = definitions.find(accumulator);
    if (innerPhi == definitions.end() ||
        innerPhi->second.location.block != inner->headerIndex ||
        innerPhi->second.instruction->opcode() != "phi") {
      return std::nullopt;
    }
    std::string outerAccumulator;
    bool hasBackedge = false;
    const auto &operands = innerPhi->second.instruction->operands();
    for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
      if (operands[index + 1] == inner->preheader) {
        outerAccumulator = operands[index];
      } else if (operands[index + 1] == inner->latch &&
                 operands[index] == addInstruction->result()) {
        hasBackedge = true;
      }
    }
    const auto outerPhi = definitions.find(outerAccumulator);
    if (outerAccumulator.empty() || !hasBackedge ||
        outerPhi == definitions.end() ||
        outerPhi->second.location.block != outer->headerIndex ||
        outerPhi->second.instruction->opcode() != "phi") {
      return std::nullopt;
    }
    bool outerBackedge = false;
    const auto &outerOperands = outerPhi->second.instruction->operands();
    for (std::size_t index = 1; index + 1 < outerOperands.size(); index += 2) {
      if (outerOperands[index + 1] == outer->latch &&
          outerOperands[index] == accumulator) {
        outerBackedge = true;
      }
    }
    if (!outerBackedge) {
      return std::nullopt;
    }
    accumulator = outerAccumulator;
  }
  const auto loadFound = definitions.find(matrixLoad->result());
  if (loadFound == definitions.end()) {
    return std::nullopt;
  }
  return ReductionPattern{
      matrixPath->base,    accumulator, loadFound->second.location,
      accumulatorInMemory, *outer,      *inner};
}

bool isDirectGlobal(const std::string &value,
                    const std::unordered_map<std::string, ArrayType> &arrays) {
  return arrays.find(value) != arrays.end();
}

bool factorWriteIsOutsideCallLoop(const CFG &cfg,
                                  const CanonicalLoop &callLoop,
                                  std::size_t block) {
  if (callLoop.blocks.find(block) != callLoop.blocks.end()) {
    return false;
  }
  // A store that is not reachable from the call-loop header is before the
  // first call.  A store reachable from the header is safe only after the
  // unique loop exit; otherwise it can change the factor between calls.
  if (!reachable(cfg, callLoop.headerIndex, block)) {
    return true;
  }
  return dominates(cfg, callLoop.exitIndex, block);
}

bool factorIsStableDuringCallLoop(const Module &module, std::size_t callerIndex,
                                  const CanonicalLoop &callLoop,
                                  const std::string &factor,
                                  const std::string &transformName,
                                  const analysis::FunctionEffectAnalysis &effects) {
  for (std::size_t functionIndex = 0; functionIndex < module.functions().size();
       ++functionIndex) {
    const auto &function = module.functions()[functionIndex];
    const auto definitions = buildDefinitions(function);
    const auto &cfg = functionIndex == callerIndex
                          ? buildCFG(function)
                          : CFG{};
    for (std::size_t block = 0; block < function.blocks().size(); ++block) {
      const auto &instructions = function.blocks()[block].instructions();
      for (const auto &instruction : instructions) {
        if (functionIndex == callerIndex &&
            callLoop.blocks.find(block) != callLoop.blocks.end() &&
            instruction.opcode() == "call" &&
            (instruction.operands().empty() ||
             instruction.operands()[0] != "@" + transformName)) {
          if (instruction.operands().empty() ||
              instruction.operands()[0].size() < 2 ||
              instruction.operands()[0].front() != '@' ||
              !effects.isReadNone(instruction.operands()[0].substr(1))) {
            return false;
          }
        }
        for (std::size_t operand = 0; operand < instruction.operands().size();
             ++operand) {
          const auto path = pointerPath(instruction.operands()[operand],
                                        definitions);
          if (!path.has_value() || path->base != factor) {
            continue;
          }
          if ((isGep(instruction.opcode()) && operand == 0) ||
              (instruction.opcode() == "load" && operand == 1)) {
            continue;
          }
          if (instruction.opcode() == "call" && operand > 0 &&
              !instruction.operands().empty() &&
              instruction.operands()[0] == "@" + transformName) {
            continue;
          }
          if (instruction.opcode() == "call" &&
              !instruction.operands().empty() &&
              instruction.operands()[0] != "@" + transformName &&
              instruction.operands()[0].size() >= 2 &&
              instruction.operands()[0].front() == '@' &&
              effects.isReadNone(instruction.operands()[0].substr(1))) {
            continue;
          }
          if (instruction.opcode() == "store" && operand == 2 &&
              functionIndex == callerIndex &&
              factorWriteIsOutsideCallLoop(cfg, callLoop, block)) {
            continue;
          }
          // Any pointer escape, call, return, or store in the repeated region
          // would invalidate a coefficient table built before the loop.
          return false;
        }
      }
    }
  }
  return true;
}

std::optional<CollapsePlan> buildPlan(Module &module,
                                      const TransformPattern &transform) {
  const auto arrays = collectGlobalArrays(module);
  const analysis::FunctionEffectAnalysis effects(module);
  std::optional<CollapsePlan> plan;
  const auto &callee = module.functions()[transform.functionIndex];
  for (std::size_t callerIndex = 0; callerIndex < module.functions().size();
       ++callerIndex) {
    if (callerIndex == transform.functionIndex) {
      continue;
    }
    const auto &caller = module.functions()[callerIndex];
    const auto definitions = buildDefinitions(caller);
    const auto cfg = buildCFG(caller);
    std::vector<CallSite> calls;
    for (std::size_t block = 0; block < caller.blocks().size(); ++block) {
      const auto &instructions = caller.blocks()[block].instructions();
      for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto &instruction = instructions[index];
        if (instruction.opcode() != "call" || instruction.operands().empty() ||
            instruction.operands()[0] != "@" + callee.name()) {
          continue;
        }
        if (instruction.operands().size() != callee.parameters().size() + 1) {
          return std::nullopt;
        }
        std::unordered_map<std::string, std::string> actual;
        for (std::size_t parameter = 0; parameter < callee.parameters().size();
             ++parameter) {
          actual.emplace(callee.parameters()[parameter].name,
                         instruction.operands()[parameter + 1]);
        }
        const auto count = actual.find(transform.count);
        const auto factor = actual.find(transform.factor);
        const auto source = actual.find(transform.source);
        const auto destination = actual.find(transform.destination);
        if (count == actual.end() || factor == actual.end() ||
            source == actual.end() || destination == actual.end() ||
            !isDirectGlobal(factor->second, arrays) ||
            !isDirectGlobal(source->second, arrays) ||
            !isDirectGlobal(destination->second, arrays) ||
            factor->second == source->second ||
            factor->second == destination->second ||
            source->second == destination->second) {
          return std::nullopt;
        }
        calls.push_back({{block, index},
                         count->second,
                         factor->second,
                         source->second,
                         destination->second});
      }
    }
    if (calls.empty()) {
      continue;
    }
    const auto callAnchor = calls.front().location.block;
    if (plan.has_value()) {
      return std::nullopt;
    }
    const auto count = calls.front().count;
    for (const auto &call : calls) {
      if (call.count != count || call.factor != calls.front().factor) {
        return std::nullopt;
      }
    }
    std::optional<CanonicalLoop> callLoop;
    for (const auto &[name, definition] : definitions) {
      if (definition.instruction->opcode() != "phi") {
        continue;
      }
      const auto *terminator =
          caller.blocks()[definition.location.block].terminator();
      if (terminator == nullptr || terminator->operands().size() != 3) {
        continue;
      }
      const auto condition = definitions.find(terminator->operands()[0]);
      if (condition == definitions.end() ||
          condition->second.instruction->opcode() != "icmp.slt" ||
          condition->second.instruction->operands().size() != 2 ||
          condition->second.instruction->operands()[0] != name) {
        continue;
      }
      const auto loop =
          canonicalLoop(caller, cfg, definitions, name,
                        condition->second.instruction->operands()[1]);
      if (loop.has_value() &&
          loop->blocks.find(callAnchor) != loop->blocks.end()) {
        if (callLoop.has_value()) {
          return std::nullopt;
        }
        callLoop = loop;
      }
    }
    if (!callLoop.has_value()) {
      return std::nullopt;
    }
    for (const auto &call : calls) {
      if (callLoop->blocks.find(call.location.block) == callLoop->blocks.end()) {
        return std::nullopt;
      }
    }
    for (std::size_t first = 0; first < calls.size(); ++first) {
      for (std::size_t second = first + 1; second < calls.size(); ++second) {
        if (calls[first].location.block == calls[second].location.block) {
          continue;
        }
        if (!dominates(cfg, calls[first].location.block,
                       calls[second].location.block)) {
          return std::nullopt;
        }
      }
    }
    for (const auto block : callLoop->blocks) {
      for (const auto &instruction : caller.blocks()[block].instructions()) {
        if (instruction.opcode() == "call" &&
            (instruction.operands().empty() ||
             instruction.operands()[0] != "@" + callee.name())) {
          if (instruction.operands().empty() ||
              instruction.operands()[0].size() < 2 ||
              instruction.operands()[0].front() != '@' ||
              !effects.isReadNone(instruction.operands()[0].substr(1))) {
            return std::nullopt;
          }
        }
      }
    }
    CollapsePlan candidate;
    candidate.callerIndex = callerIndex;
    candidate.transform = transform;
    candidate.callLoop = *callLoop;
    candidate.calls = calls;
    long long staticTripCount = 0;
    const bool repeatedCall =
        calls.size() >= 2 ||
        (detail::parseIntegerLiteral(callLoop->bound, staticTripCount) &&
         staticTripCount > 1);
    // The table is the suffix-product form of the affine recurrence.  It is
    // legal only when the same factor object is stable for every transformed
    // call; readnone calls in the loop are harmless, while unknown effects or
    // an escaping factor force the ordinary row-summary path.
    candidate.precomputeCoefficients =
        repeatedCall &&
        factorIsStableDuringCallLoop(module, callerIndex, *callLoop,
                                     calls.front().factor, callee.name(),
                                     effects);
    for (const auto &call : calls) {
      candidate.matrices.emplace(call.source, arrays.at(call.source));
      candidate.matrices.emplace(call.destination, arrays.at(call.destination));
      if (arrays.at(call.factor).columns != transform.columns ||
          arrays.at(call.source).columns != transform.columns ||
          arrays.at(call.destination).columns != transform.columns ||
          arrays.at(call.source).rows != arrays.at(call.destination).rows) {
        return std::nullopt;
      }
    }
    if (candidate.matrices.find(calls.front().factor) !=
        candidate.matrices.end()) {
      return std::nullopt;
    }
    std::unordered_map<std::string, ReductionPattern> reductions;
    for (std::size_t block = 0; block < caller.blocks().size(); ++block) {
      for (std::size_t index = 0;
           index < caller.blocks()[block].instructions().size(); ++index) {
        const auto reduction =
            analyzeReduction(caller, cfg, definitions, {block, index}, count);
        if (reduction.has_value() &&
            candidate.matrices.find(reduction->matrix) !=
                candidate.matrices.end()) {
          if (!dominates(cfg, callLoop->exitIndex,
                         reduction->outer.headerIndex) ||
              !reductions.emplace(reduction->matrix, *reduction).second) {
            return std::nullopt;
          }
        }
      }
    }
    candidate.reductions = reductions;
    for (std::size_t functionIndex = 0;
         functionIndex < module.functions().size(); ++functionIndex) {
      const auto &function = module.functions()[functionIndex];
      const auto functionDefinitions = buildDefinitions(function);
      for (std::size_t block = 0; block < function.blocks().size(); ++block) {
        const auto &instructions = function.blocks()[block].instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
          const auto &instruction = instructions[index];
          for (std::size_t operand = 0; operand < instruction.operands().size();
               ++operand) {
            const auto path = pointerPath(instruction.operands()[operand],
                                          functionDefinitions);
            if (!path.has_value() || candidate.matrices.find(path->base) ==
                                         candidate.matrices.end()) {
              continue;
            }
            if (functionIndex == transform.functionIndex &&
                (path->base == transform.source ||
                 path->base == transform.destination)) {
              continue;
            }
            if (functionIndex == callerIndex &&
                instruction.opcode() == "call" &&
                instruction.operands()[0] == "@" + callee.name()) {
              continue;
            }
            if (functionIndex != callerIndex ||
                !((isGep(instruction.opcode()) && operand == 0) ||
                  (instruction.opcode() == "store" && operand == 2) ||
                  (instruction.opcode() == "load" && operand == 1))) {
              return std::nullopt;
            }
            if (instruction.opcode() == "load") {
              const auto reduction = candidate.reductions.find(path->base);
              if (reduction == candidate.reductions.end() ||
                  reduction->second.matrixLoad.block != block ||
                  reduction->second.matrixLoad.instruction != index) {
                return std::nullopt;
              }
            } else if (instruction.opcode() == "store" &&
                       reachable(cfg, callLoop->headerIndex, block)) {
              return std::nullopt;
            }
          }
        }
      }
    }
    plan = std::move(candidate);
  }
  return plan;
}

std::string freshSymbol(const Module &module, const std::string &prefix) {
  std::unordered_set<std::string> symbols;
  for (const auto &global : module.globals()) {
    symbols.insert(global.name());
  }
  for (const auto &function : module.functions()) {
    symbols.insert(function.name());
  }
  for (std::size_t index = 0;; ++index) {
    const auto candidate = prefix + std::to_string(index);
    if (symbols.find(candidate) == symbols.end()) {
      return candidate;
    }
  }
}

Function buildSummarizeFunction(const std::string &name, std::size_t columns) {
  Function function(name, "void",
                    {{"%count", "int"},
                     {"%matrix", "int[][" + std::to_string(columns) + "]"},
                     {"%summary", "int[]"}});
  BasicBlock entry("summary.init.entry");
  entry.addInstruction(Instruction{"br", {"summary.init.row.cond"}});
  function.addBlock(std::move(entry));
  BasicBlock rowCond("summary.init.row.cond");
  rowCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "summary.init.entry", "%summary.init.row.next",
                   "summary.init.row.latch"},
                  "%summary.init.row"});
  rowCond.addInstruction(Instruction{
      "icmp.slt", {"%summary.init.row", "%count"}, "%summary.init.row.more"});
  rowCond.addInstruction(
      Instruction{"br",
                  {"%summary.init.row.more", "summary.init.row.body",
                   "summary.init.exit"}});
  function.addBlock(std::move(rowCond));
  BasicBlock rowBody("summary.init.row.body");
  rowBody.addInstruction(Instruction{"br", {"summary.init.column.cond"}});
  function.addBlock(std::move(rowBody));
  BasicBlock columnCond("summary.init.column.cond");
  columnCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "summary.init.row.body",
                   "%summary.init.column.next", "summary.init.column.body"},
                  "%summary.init.column"});
  columnCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "summary.init.row.body",
                   "%summary.init.sum.next", "summary.init.column.body"},
                  "%summary.init.sum"});
  columnCond.addInstruction(Instruction{"icmp.slt",
                                        {"%summary.init.column", "%count"},
                                        "%summary.init.column.more"});
  columnCond.addInstruction(
      Instruction{"br",
                  {"%summary.init.column.more", "summary.init.column.body",
                   "summary.init.row.latch"}});
  function.addBlock(std::move(columnCond));
  BasicBlock columnBody("summary.init.column.body");
  columnBody.addInstruction(Instruction{"getelementptr",
                                        {"%matrix", "%summary.init.row"},
                                        "%summary.init.matrix.row"});
  columnBody.addInstruction(
      Instruction{"getelementptr",
                  {"%summary.init.matrix.row", "%summary.init.column"},
                  "%summary.init.matrix.element"});
  columnBody.addInstruction(Instruction{"load",
                                        {"int", "%summary.init.matrix.element"},
                                        "%summary.init.element"});
  columnBody.addInstruction(
      Instruction{"add",
                  {"%summary.init.sum", "%summary.init.element"},
                  "%summary.init.sum.next"});
  columnBody.addInstruction(Instruction{
      "add", {"%summary.init.column", "1"}, "%summary.init.column.next"});
  columnBody.addInstruction(Instruction{"br", {"summary.init.column.cond"}});
  function.addBlock(std::move(columnBody));
  BasicBlock rowLatch("summary.init.row.latch");
  rowLatch.addInstruction(Instruction{"getelementptr",
                                      {"%summary", "%summary.init.row"},
                                      "%summary.init.output"});
  rowLatch.addInstruction(Instruction{
      "store", {"int", "%summary.init.sum", "%summary.init.output"}});
  rowLatch.addInstruction(
      Instruction{"add", {"%summary.init.row", "1"}, "%summary.init.row.next"});
  rowLatch.addInstruction(Instruction{"br", {"summary.init.row.cond"}});
  function.addBlock(std::move(rowLatch));
  BasicBlock exit("summary.init.exit");
  exit.addInstruction(Instruction{"ret", {}});
  function.addBlock(std::move(exit));
  return function;
}

Function buildCoefficientFunction(const std::string &name,
                                  std::size_t columns,
                                  bool skipsUnitFactor) {
  Function function(name, "void",
                    {{"%count", "int"},
                     {"%factor", "int[][" + std::to_string(columns) + "]"},
                     {"%coefficient",
                      "int[][" + std::to_string(columns) + "]"}});
  BasicBlock entry("coefficient.init.entry");
  entry.addInstruction(Instruction{
      "icmp.sgt", {"%count", "0"}, "%coefficient.init.positive"});
  entry.addInstruction(Instruction{
      "br", {"%coefficient.init.positive", "coefficient.init.row.cond",
             "coefficient.init.exit"}});
  function.addBlock(std::move(entry));

  BasicBlock rowCond("coefficient.init.row.cond");
  rowCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "coefficient.init.entry",
                   "%coefficient.init.row.next", "coefficient.init.row.latch"},
                  "%coefficient.init.row"});
  rowCond.addInstruction(Instruction{
      "icmp.slt", {"%coefficient.init.row", "%count"},
      "%coefficient.init.row.more"});
  rowCond.addInstruction(Instruction{
      "br", {"%coefficient.init.row.more", "coefficient.init.row.body",
             "coefficient.init.exit"}});
  function.addBlock(std::move(rowCond));

  BasicBlock rowBody("coefficient.init.row.body");
  rowBody.addInstruction(Instruction{
      "getelementptr", {"%factor", "%coefficient.init.row"},
      "%coefficient.init.factor.row"});
  rowBody.addInstruction(Instruction{
      "getelementptr", {"%coefficient", "%coefficient.init.row"},
      "%coefficient.init.output.row"});
  rowBody.addInstruction(Instruction{
      "sub", {"%count", "1"}, "%coefficient.init.stage.initial"});
  rowBody.addInstruction(Instruction{"br", {"coefficient.init.stage.cond"}});
  function.addBlock(std::move(rowBody));

  BasicBlock stageCond("coefficient.init.stage.cond");
  stageCond.addInstruction(
      Instruction{"phi",
                  {"int", "%coefficient.init.stage.initial",
                   "coefficient.init.row.body", "%coefficient.init.stage.next",
                   "coefficient.init.stage.latch"},
                  "%coefficient.init.stage"});
  stageCond.addInstruction(
      Instruction{"phi",
                  {"int", "1", "coefficient.init.row.body",
                   "%coefficient.init.suffix.next",
                   "coefficient.init.stage.latch"},
                  "%coefficient.init.suffix"});
  stageCond.addInstruction(Instruction{
      "icmp.sge", {"%coefficient.init.stage", "0"},
      "%coefficient.init.stage.more"});
  stageCond.addInstruction(Instruction{
      "br", {"%coefficient.init.stage.more", "coefficient.init.stage.body",
             "coefficient.init.row.latch"}});
  function.addBlock(std::move(stageCond));

  BasicBlock stageBody("coefficient.init.stage.body");
  stageBody.addInstruction(Instruction{
      "getelementptr", {"%coefficient.init.factor.row",
                         "%coefficient.init.stage"},
      "%coefficient.init.factor.pointer"});
  stageBody.addInstruction(Instruction{
      "load", {"int", "%coefficient.init.factor.pointer"},
      "%coefficient.init.factor.value"});
  if (skipsUnitFactor) {
    stageBody.addInstruction(Instruction{
        "icmp.eq", {"%coefficient.init.factor.value", "1"},
        "%coefficient.init.factor.is.unit"});
    stageBody.addInstruction(Instruction{
        "br", {"%coefficient.init.factor.is.unit", "coefficient.init.skip",
               "coefficient.init.apply"}});
  } else {
    stageBody.addInstruction(Instruction{"br", {"coefficient.init.apply"}});
  }
  function.addBlock(std::move(stageBody));

  if (skipsUnitFactor) {
    BasicBlock skip("coefficient.init.skip");
    skip.addInstruction(Instruction{
        "getelementptr", {"%coefficient.init.output.row",
                           "%coefficient.init.stage"},
        "%coefficient.init.skip.pointer"});
    skip.addInstruction(Instruction{
        "store", {"int", "0", "%coefficient.init.skip.pointer"}});
    skip.addInstruction(Instruction{"br", {"coefficient.init.stage.latch"}});
    function.addBlock(std::move(skip));
  }

  BasicBlock apply("coefficient.init.apply");
  apply.addInstruction(Instruction{
      "getelementptr", {"%coefficient.init.output.row",
                         "%coefficient.init.stage"},
      "%coefficient.init.apply.pointer"});
  apply.addInstruction(Instruction{
      "store", {"int", "%coefficient.init.suffix",
                 "%coefficient.init.apply.pointer"}});
  apply.addInstruction(Instruction{
      "mul", {"%coefficient.init.suffix", "%coefficient.init.factor.value"},
      "%coefficient.init.suffix.updated"});
  apply.addInstruction(Instruction{"br", {"coefficient.init.stage.latch"}});
  function.addBlock(std::move(apply));

  BasicBlock stageLatch("coefficient.init.stage.latch");
  if (skipsUnitFactor) {
    stageLatch.addInstruction(
        Instruction{"phi",
                    {"int", "%coefficient.init.suffix.updated",
                     "coefficient.init.apply", "%coefficient.init.suffix",
                     "coefficient.init.skip"},
                    "%coefficient.init.suffix.next"});
  } else {
    stageLatch.addInstruction(Instruction{
        "add", {"%coefficient.init.suffix.updated", "0"},
        "%coefficient.init.suffix.next"});
  }
  stageLatch.addInstruction(Instruction{
      "sub", {"%coefficient.init.stage", "1"},
      "%coefficient.init.stage.next"});
  stageLatch.addInstruction(Instruction{"br", {"coefficient.init.stage.cond"}});
  function.addBlock(std::move(stageLatch));

  BasicBlock rowLatch("coefficient.init.row.latch");
  rowLatch.addInstruction(Instruction{
      "add", {"%coefficient.init.row", "1"}, "%coefficient.init.row.next"});
  rowLatch.addInstruction(Instruction{"br", {"coefficient.init.row.cond"}});
  function.addBlock(std::move(rowLatch));

  BasicBlock exit("coefficient.init.exit");
  exit.addInstruction(Instruction{"ret", {}});
  function.addBlock(std::move(exit));
  return function;
}

Function buildCoefficientTransformFunction(const std::string &name,
                                            std::size_t columns) {
  Function function(name, "void",
                    {{"%count", "int"},
                     {"%coefficient",
                      "int[][" + std::to_string(columns) + "]"},
                     {"%source", "int[]"},
                     {"%destination", "int[]"}});
  BasicBlock entry("coefficient.update.entry");
  entry.addInstruction(Instruction{"br", {"coefficient.update.row.cond"}});
  function.addBlock(std::move(entry));

  BasicBlock rowCond("coefficient.update.row.cond");
  rowCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "coefficient.update.entry",
                   "%coefficient.update.row.next", "coefficient.update.row.latch"},
                  "%coefficient.update.row"});
  rowCond.addInstruction(Instruction{
      "icmp.slt", {"%coefficient.update.row", "%count"},
      "%coefficient.update.row.more"});
  rowCond.addInstruction(Instruction{
      "br", {"%coefficient.update.row.more", "coefficient.update.row.body",
             "coefficient.update.exit"}});
  function.addBlock(std::move(rowCond));

  BasicBlock rowBody("coefficient.update.row.body");
  rowBody.addInstruction(Instruction{
      "getelementptr", {"%coefficient", "%coefficient.update.row"},
      "%coefficient.update.coefficient.row"});
  rowBody.addInstruction(Instruction{"br", {"coefficient.update.stage.cond"}});
  function.addBlock(std::move(rowBody));

  BasicBlock stageCond("coefficient.update.stage.cond");
  stageCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "coefficient.update.row.body",
                   "%coefficient.update.stage.next",
                   "coefficient.update.stage.latch"},
                  "%coefficient.update.stage"});
  stageCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "coefficient.update.row.body",
                   "%coefficient.update.accum.next",
                   "coefficient.update.stage.latch"},
                  "%coefficient.update.accum"});
  stageCond.addInstruction(Instruction{
      "icmp.slt", {"%coefficient.update.stage", "%count"},
      "%coefficient.update.stage.more"});
  stageCond.addInstruction(Instruction{
      "br", {"%coefficient.update.stage.more", "coefficient.update.stage.body",
             "coefficient.update.row.latch"}});
  function.addBlock(std::move(stageCond));

  BasicBlock stageBody("coefficient.update.stage.body");
  stageBody.addInstruction(Instruction{
      "getelementptr", {"%coefficient.update.coefficient.row",
                         "%coefficient.update.stage"},
      "%coefficient.update.coefficient.pointer"});
  stageBody.addInstruction(Instruction{
      "load", {"int", "%coefficient.update.coefficient.pointer"},
      "%coefficient.update.coefficient.value"});
  stageBody.addInstruction(Instruction{
      "getelementptr", {"%source", "%coefficient.update.stage"},
      "%coefficient.update.source.pointer"});
  stageBody.addInstruction(Instruction{
      "load", {"int", "%coefficient.update.source.pointer"},
      "%coefficient.update.source.value"});
  stageBody.addInstruction(Instruction{
      "mul", {"%coefficient.update.coefficient.value",
               "%coefficient.update.source.value"},
      "%coefficient.update.product"});
  stageBody.addInstruction(Instruction{
      "add", {"%coefficient.update.accum", "%coefficient.update.product"},
      "%coefficient.update.accum.next"});
  stageBody.addInstruction(Instruction{"br", {"coefficient.update.stage.latch"}});
  function.addBlock(std::move(stageBody));

  BasicBlock stageLatch("coefficient.update.stage.latch");
  stageLatch.addInstruction(Instruction{
      "add", {"%coefficient.update.stage", "1"},
      "%coefficient.update.stage.next"});
  stageLatch.addInstruction(Instruction{
      "br", {"coefficient.update.stage.cond"}});
  function.addBlock(std::move(stageLatch));

  BasicBlock rowLatch("coefficient.update.row.latch");
  rowLatch.addInstruction(Instruction{
      "getelementptr", {"%destination", "%coefficient.update.row"},
      "%coefficient.update.destination.pointer"});
  rowLatch.addInstruction(Instruction{
      "store", {"int", "%coefficient.update.accum",
                 "%coefficient.update.destination.pointer"}});
  rowLatch.addInstruction(Instruction{
      "add", {"%coefficient.update.row", "1"},
      "%coefficient.update.row.next"});
  rowLatch.addInstruction(Instruction{"br", {"coefficient.update.row.cond"}});
  function.addBlock(std::move(rowLatch));

  BasicBlock exit("coefficient.update.exit");
  exit.addInstruction(Instruction{"ret", {}});
  function.addBlock(std::move(exit));
  return function;
}

Function buildTransformFunction(const std::string &name, std::size_t columns,
                                bool skipsUnitFactor) {
  Function function(name, "void",
                    {{"%count", "int"},
                     {"%factor", "int[][" + std::to_string(columns) + "]"},
                     {"%source", "int[]"},
                     {"%destination", "int[]"}});
  BasicBlock entry("summary.update.entry");
  entry.addInstruction(Instruction{"br", {"summary.update.row.cond"}});
  function.addBlock(std::move(entry));
  BasicBlock rowCond("summary.update.row.cond");
  rowCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "summary.update.entry",
                   "%summary.update.row.next", "summary.update.row.latch"},
                  "%summary.update.row"});
  rowCond.addInstruction(Instruction{"icmp.slt",
                                     {"%summary.update.row", "%count"},
                                     "%summary.update.row.more"});
  rowCond.addInstruction(
      Instruction{"br",
                  {"%summary.update.row.more", "summary.update.row.body",
                   "summary.update.exit"}});
  function.addBlock(std::move(rowCond));
  BasicBlock rowBody("summary.update.row.body");
  rowBody.addInstruction(Instruction{"getelementptr",
                                     {"%factor", "%summary.update.row"},
                                     "%summary.update.factor.row"});
  rowBody.addInstruction(Instruction{"br", {"summary.update.stage.cond"}});
  function.addBlock(std::move(rowBody));
  BasicBlock stageCond("summary.update.stage.cond");
  stageCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "summary.update.row.body",
                   "%summary.update.stage.next", "summary.update.stage.latch"},
                  "%summary.update.stage"});
  stageCond.addInstruction(
      Instruction{"phi",
                  {"int", "0", "summary.update.row.body",
                   "%summary.update.accum.next", "summary.update.stage.latch"},
                  "%summary.update.accum"});
  stageCond.addInstruction(Instruction{"icmp.slt",
                                       {"%summary.update.stage", "%count"},
                                       "%summary.update.stage.more"});
  stageCond.addInstruction(
      Instruction{"br",
                  {"%summary.update.stage.more", "summary.update.stage.body",
                   "summary.update.row.latch"}});
  function.addBlock(std::move(stageCond));
  BasicBlock stageBody("summary.update.stage.body");
  stageBody.addInstruction(Instruction{"getelementptr",
                                       {"%summary.update.factor.row",
                                        "%summary.update.stage"},
                                       "%summary.update.factor.pointer"});
  stageBody.addInstruction(Instruction{
      "load", {"int", "%summary.update.factor.pointer"},
      "%summary.update.factor.value"});
  if (skipsUnitFactor) {
    stageBody.addInstruction(Instruction{
        "icmp.eq", {"%summary.update.factor.value", "1"},
        "%summary.update.skip"});
    stageBody.addInstruction(
        Instruction{"br",
                    {"%summary.update.skip", "summary.update.stage.latch",
                     "summary.update.apply"}});
  } else {
    stageBody.addInstruction(Instruction{"br", {"summary.update.apply"}});
  }
  function.addBlock(std::move(stageBody));
  BasicBlock apply("summary.update.apply");
  apply.addInstruction(Instruction{"getelementptr",
                                   {"%source", "%summary.update.stage"},
                                   "%summary.update.source.pointer"});
  apply.addInstruction(Instruction{"load",
                                   {"int", "%summary.update.source.pointer"},
                                   "%summary.update.source.value"});
  apply.addInstruction(Instruction{
      "mul",
      {"%summary.update.accum", "%summary.update.factor.value"},
      "%summary.update.scaled"});
  apply.addInstruction(
      Instruction{"add",
                  {"%summary.update.scaled", "%summary.update.source.value"},
                  "%summary.update.next.value"});
  apply.addInstruction(Instruction{"br", {"summary.update.stage.latch"}});
  function.addBlock(std::move(apply));
  BasicBlock stageLatch("summary.update.stage.latch");
  if (skipsUnitFactor) {
    stageLatch.addInstruction(
        Instruction{"phi",
                    {"int", "%summary.update.accum",
                     "summary.update.stage.body",
                     "%summary.update.next.value", "summary.update.apply"},
                    "%summary.update.accum.next"});
  } else {
    stageLatch.addInstruction(
        Instruction{"phi",
                    {"int", "%summary.update.next.value",
                     "summary.update.apply"},
                    "%summary.update.accum.next"});
  }
  stageLatch.addInstruction(Instruction{
      "add", {"%summary.update.stage", "1"}, "%summary.update.stage.next"});
  stageLatch.addInstruction(Instruction{"br", {"summary.update.stage.cond"}});
  function.addBlock(std::move(stageLatch));
  BasicBlock rowLatch("summary.update.row.latch");
  rowLatch.addInstruction(Instruction{"getelementptr",
                                      {"%destination", "%summary.update.row"},
                                      "%summary.update.destination.pointer"});
  rowLatch.addInstruction(Instruction{
      "store", {"int", "%summary.update.accum",
                 "%summary.update.destination.pointer"}});
  rowLatch.addInstruction(Instruction{
      "add", {"%summary.update.row", "1"}, "%summary.update.row.next"});
  rowLatch.addInstruction(Instruction{"br", {"summary.update.row.cond"}});
  function.addBlock(std::move(rowLatch));
  BasicBlock exit("summary.update.exit");
  exit.addInstruction(Instruction{"ret", {}});
  function.addBlock(std::move(exit));
  return function;
}

void applyPlan(Module &module, const CollapsePlan &plan) {
  const auto originalTransformName =
      module.functions()[plan.transform.functionIndex].name();
  const auto arrays = collectGlobalArrays(module);
  const bool useCoefficients =
      plan.precomputeCoefficients &&
      std::getenv("COMPILER_DISABLE_ARRAY_SUMMARY_COEFFICIENT_PRECOMPUTE") ==
          nullptr;
  const auto initName = freshSymbol(module, "__array_summary_init_");
  module.addFunction(buildSummarizeFunction(initName, plan.transform.columns));
  std::string coefficientInitName;
  std::string coefficientGlobal;
  if (useCoefficients) {
    coefficientInitName = freshSymbol(module, "__array_summary_coeff_init_");
    module.addFunction(buildCoefficientFunction(coefficientInitName,
                                                plan.transform.columns,
                                                plan.transform.skipsUnitFactor));
  }
  const auto updateName = freshSymbol(module, "__array_summary_update_");
  if (useCoefficients) {
    module.addFunction(buildCoefficientTransformFunction(
        updateName, plan.transform.columns));
  } else {
    module.addFunction(buildTransformFunction(updateName, plan.transform.columns,
                                              plan.transform.skipsUnitFactor));
  }
  std::unordered_map<std::string, std::string> summaries;
  for (const auto &[matrix, type] : plan.matrices) {
    const auto globalName = freshSymbol(module, "__array_summary_rows_");
    module.addGlobal(GlobalVariable(
        globalName, "int[" + std::to_string(type.rows) + "]",
        Constant::aggregate("int[" + std::to_string(type.rows) + "]", {})));
    summaries.emplace(matrix, "@" + globalName);
  }
  if (useCoefficients) {
    const auto &factorType = arrays.at(plan.calls.front().factor);
    const auto globalName = freshSymbol(module, "__array_summary_coeff_");
    module.addGlobal(GlobalVariable(
        globalName,
        "int[" + std::to_string(factorType.rows) + "][" +
            std::to_string(plan.transform.columns) + "]",
        Constant::aggregate(
            "int[" + std::to_string(factorType.rows) + "][" +
                std::to_string(plan.transform.columns) + "]",
            {})));
    coefficientGlobal = "@" + globalName;
  }
  auto &caller = module.functions()[plan.callerIndex];
  auto &preheader =
      caller.blocks()[plan.callLoop.preheaderIndex].instructions();
  const auto terminator = preheader.back();
  preheader.pop_back();
  std::unordered_set<std::string> initialized;
  std::unordered_set<std::string> defined;
  for (const auto &call : plan.calls) {
    if (defined.find(call.source) == defined.end()) {
      initialized.insert(call.source);
    }
    defined.insert(call.destination);
  }
  for (const auto &[matrix, reduction] : plan.reductions) {
    (void)reduction;
    initialized.insert(matrix);
  }
  if (useCoefficients) {
    preheader.emplace_back(
        "call", std::vector<std::string>{
                    "@" + coefficientInitName, plan.calls.front().count,
                    plan.calls.front().factor, coefficientGlobal});
  }
  for (const auto &matrix : initialized) {
    const auto &summary = summaries.at(matrix);
    preheader.emplace_back(
        "call", std::vector<std::string>{
                    "@" + initName, plan.calls.front().count, matrix, summary});
  }
  preheader.push_back(terminator);
  for (const auto &call : plan.calls) {
    auto &instruction = caller.blocks()[call.location.block]
                            .instructions()[call.location.instruction];
    if (useCoefficients) {
      instruction = Instruction{
          "call", {"@" + updateName, call.count, coefficientGlobal,
                   summaries.at(call.source), summaries.at(call.destination)}};
    } else {
      instruction = Instruction{"call",
                                {"@" + updateName, call.count, call.factor,
                                 summaries.at(call.source),
                                 summaries.at(call.destination)}};
    }
  }
  std::unordered_set<std::string> names;
  for (const auto &block : caller.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  std::size_t counter = 0;
  const auto freshValue = [&](const std::string &stem) mutable {
    while (true) {
      const auto value =
          "%array.summary." + stem + "." + std::to_string(counter++);
      if (names.insert(value).second) {
        return value;
      }
    }
  };
  for (const auto &[matrix, reduction] : plan.reductions) {
    auto valueName = freshValue;
    const auto pointer = valueName("pointer");
    const auto rowSum = valueName("row");
    const auto oldSum = reduction.accumulatorInMemory ? valueName("old")
                                                      : reduction.accumulator;
    const auto nextSum = valueName("next");
    auto &body = caller.blocks()[reduction.inner.preheaderIndex].instructions();
    body.clear();
    body.emplace_back("getelementptr",
                      std::vector<std::string>{summaries.at(matrix),
                                               reduction.outer.induction},
                      pointer);
    body.emplace_back("load", std::vector<std::string>{"int", pointer}, rowSum);
    if (reduction.accumulatorInMemory) {
      body.emplace_back("load",
                        std::vector<std::string>{"int", reduction.accumulator},
                        oldSum);
    }
    body.emplace_back("add", std::vector<std::string>{oldSum, rowSum}, nextSum);
    if (reduction.accumulatorInMemory) {
      body.emplace_back("store", std::vector<std::string>{
                                     "int", nextSum, reduction.accumulator});
    } else {
      auto &header =
          caller.blocks()[reduction.outer.headerIndex].instructions();
      for (auto &instruction : header) {
        if (instruction.opcode() != "phi" ||
            instruction.result() != reduction.accumulator) {
          continue;
        }
        auto operands = instruction.operands();
        for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
          if (operands[index + 1] == reduction.outer.latch) {
            operands[index] = nextSum;
          }
        }
        instruction =
            Instruction{"phi", std::move(operands), instruction.result()};
      }
    }
    body.emplace_back("br", std::vector<std::string>{reduction.inner.exit});
  }
  module.functions().erase(
      std::remove_if(module.functions().begin(), module.functions().end(),
                     [&](const Function &function) {
                       return function.name() == originalTransformName;
                     }),
      module.functions().end());
  module.addPassNote(
      "collapsed proven two-dimensional affine updates to row summaries");
}

} // namespace

void ArraySummaryDimensionCollapsePass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_ARRAY_SUMMARY_DIMENSION_COLLAPSE") !=
      nullptr) {
    return;
  }
  for (std::size_t function = 0; function < module.functions().size();
       ++function) {
    const auto transform = analyzeTransform(module, function);
    if (!transform.has_value()) {
      continue;
    }
    const auto plan = buildPlan(module, *transform);
    if (!plan.has_value()) {
      continue;
    }
    applyPlan(module, *plan);
    return;
  }
}

} // namespace compiler::midend::pass
