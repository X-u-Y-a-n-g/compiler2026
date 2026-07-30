#include "../../../include/compiler/midend/pass/IterationScalarResidencyPass.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
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

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> successors;
  std::vector<std::vector<std::string>> predecessors;
};

struct Definition {
  const Instruction *instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct AccessPath {
  std::string root;
  std::vector<std::string> indices;
};

struct BaseOffset {
  std::string base;
  std::int64_t offset = 0;
};

struct IntegerRange {
  std::int64_t minimum = 0;
  std::int64_t maximum = 0;
  bool valid = false;
};

enum class InductionDirection { Increasing, Decreasing };

struct LoopInfo {
  std::string header;
  std::string body;
  std::string exit;
  std::string preheader;
  std::unordered_set<std::string> region;
  std::vector<std::string> backedges;
  std::string induction;
  std::string initial;
  std::string bound;
  InductionDirection direction = InductionDirection::Increasing;
};

struct ParameterObject {
  std::string object;
  bool valid = true;
  bool initialized = false;
};

using FunctionParameterObjects =
    std::unordered_map<std::string,
                       std::unordered_map<std::string, ParameterObject>>;

struct Candidate {
  LoopInfo loop;
  AccessPath path;
  std::string type;
  std::string representativeAddress;
  std::size_t loads = 0;
  std::size_t stores = 0;
};

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
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
  cfg.successors.resize(function.blocks().size());
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    for (const auto &target : branchTargets(function.blocks()[index])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end())
        continue;
      cfg.successors[index].push_back(target);
      cfg.predecessors[found->second].push_back(
          function.blocks()[index].name());
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function &function) {
  DefinitionMap definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
      }
    }
  }
  return definitions;
}

const BasicBlock *findBlock(const Function &function, const CFG &cfg,
                            const std::string &name) {
  const auto found = cfg.index.find(name);
  return found == cfg.index.end() ? nullptr : &function.blocks()[found->second];
}

BasicBlock *findBlock(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name)
      return &block;
  }
  return nullptr;
}

std::optional<std::unordered_set<std::string>>
collectRegion(const CFG &cfg, const std::string &body,
              const std::string &header, const std::string &exit) {
  std::unordered_set<std::string> region;
  std::vector<std::string> work{body};
  while (!work.empty()) {
    const auto current = std::move(work.back());
    work.pop_back();
    if (current == header || !region.insert(current).second)
      continue;
    if (current == exit)
      return std::nullopt;
    const auto found = cfg.index.find(current);
    if (found == cfg.index.end())
      return std::nullopt;
    if (cfg.successors[found->second].empty())
      return std::nullopt;
    for (const auto &successor : cfg.successors[found->second]) {
      if (successor == exit)
        return std::nullopt;
      if (successor != header)
        work.push_back(successor);
    }
  }
  return region;
}

bool isUnitUpdate(const Instruction &instruction, const std::string &induction,
                  InductionDirection direction) {
  if (!instruction.hasResult() || instruction.operands().size() != 2) {
    return false;
  }
  long long literal = 0;
  if (direction == InductionDirection::Increasing &&
      instruction.opcode() == "add") {
    return (instruction.operands()[0] == induction &&
            detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
            literal == 1) ||
           (instruction.operands()[1] == induction &&
            detail::parseIntegerLiteral(instruction.operands()[0], literal) &&
            literal == 1);
  }
  return direction == InductionDirection::Decreasing &&
         instruction.opcode() == "sub" &&
         instruction.operands()[0] == induction &&
         detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
         literal == 1;
}

std::optional<LoopInfo> matchLoop(const CFG &cfg,
                                  const DefinitionMap &definitions,
                                  const BasicBlock &header) {
  const auto *terminator = header.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3) {
    return std::nullopt;
  }
  const auto body = terminator->operands()[1];
  const auto exit = terminator->operands()[2];
  const auto region = collectRegion(cfg, body, header.name(), exit);
  if (!region.has_value() || region->empty())
    return std::nullopt;

  const auto condition = definitions.find(terminator->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.block != header.name() ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto &comparison = *condition->second.instruction;
  InductionDirection direction;
  if (comparison.opcode() == "icmp.slt") {
    direction = InductionDirection::Increasing;
  } else if (comparison.opcode() == "icmp.sge") {
    direction = InductionDirection::Decreasing;
  } else {
    return std::nullopt;
  }

  const auto &induction = comparison.operands()[0];
  const auto &bound = comparison.operands()[1];
  const Instruction *phi = nullptr;
  for (const auto &instruction : header.instructions()) {
    if (instruction.opcode() == "phi" && instruction.result() == induction) {
      phi = &instruction;
      break;
    }
  }
  if (phi == nullptr || phi->operands().size() < 5 ||
      phi->operands().size() % 2 == 0 || phi->operands()[0] != "int") {
    return std::nullopt;
  }

  LoopInfo loop;
  loop.header = header.name();
  loop.body = body;
  loop.exit = exit;
  loop.region = *region;
  loop.induction = induction;
  loop.bound = bound;
  loop.direction = direction;
  for (std::size_t index = 1; index + 1 < phi->operands().size(); index += 2) {
    const auto &value = phi->operands()[index];
    const auto &predecessor = phi->operands()[index + 1];
    if (region->find(predecessor) == region->end()) {
      if (!loop.preheader.empty())
        return std::nullopt;
      loop.preheader = predecessor;
      loop.initial = value;
      continue;
    }
    const auto update = definitions.find(value);
    if (update == definitions.end() ||
        region->find(update->second.block) == region->end() ||
        !isUnitUpdate(*update->second.instruction, induction, direction)) {
      return std::nullopt;
    }
    loop.backedges.push_back(predecessor);
  }
  if (loop.preheader.empty() || loop.backedges.empty())
    return std::nullopt;

  const auto headerFound = cfg.index.find(loop.header);
  const auto bodyFound = cfg.index.find(loop.body);
  if (headerFound == cfg.index.end() || bodyFound == cfg.index.end()) {
    return std::nullopt;
  }
  std::unordered_set<std::string> expected(loop.backedges.begin(),
                                           loop.backedges.end());
  expected.insert(loop.preheader);
  const std::unordered_set<std::string> actual(
      cfg.predecessors[headerFound->second].begin(),
      cfg.predecessors[headerFound->second].end());
  if (actual != expected || cfg.predecessors[bodyFound->second] !=
                                std::vector<std::string>{loop.header}) {
    return std::nullopt;
  }
  return loop;
}

std::vector<LoopInfo> collectLoops(const Function &function, const CFG &cfg,
                                   const DefinitionMap &definitions) {
  std::vector<LoopInfo> loops;
  for (const auto &block : function.blocks()) {
    if (auto loop = matchLoop(cfg, definitions, block)) {
      loops.push_back(std::move(*loop));
    }
  }
  return loops;
}

std::optional<AccessPath> accessPath(const std::string &address,
                                     const DefinitionMap &definitions) {
  AccessPath path;
  path.root = address;
  std::unordered_set<std::string> visited;
  while (!path.root.empty() && path.root.front() == '%' &&
         visited.insert(path.root).second) {
    const auto found = definitions.find(path.root);
    if (found == definitions.end())
      break;
    const auto &instruction = *found->second.instruction;
    const auto &opcode = instruction.opcode();
    if (opcode != "getelementptr" && opcode != "getelementptr.linear" &&
        opcode != "gep" && opcode != "gep.linear") {
      break;
    }
    if (instruction.operands().size() != 2)
      return std::nullopt;
    path.indices.insert(path.indices.begin(), instruction.operands()[1]);
    path.root = instruction.operands()[0];
  }
  return path;
}

bool samePath(const AccessPath &lhs, const AccessPath &rhs) {
  return lhs.root == rhs.root && lhs.indices == rhs.indices;
}

std::string pathKey(const AccessPath &path, const std::string &type) {
  std::string key = type + "\n" + path.root;
  for (const auto &index : path.indices)
    key += "\n" + index;
  return key;
}

std::optional<std::string> resolveObjectRoot(const std::string &value,
                                             const DefinitionMap &definitions) {
  std::string current = value;
  std::unordered_set<std::string> visited;
  while (!current.empty() && current.front() == '%' &&
         visited.insert(current).second) {
    const auto found = definitions.find(current);
    if (found == definitions.end())
      return std::nullopt;
    const auto &instruction = *found->second.instruction;
    if (instruction.opcode() == "alloca")
      return current;
    if ((instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "getelementptr.linear" ||
         instruction.opcode() == "gep" ||
         instruction.opcode() == "gep.linear") &&
        instruction.operands().size() == 2) {
      current = instruction.operands()[0];
      continue;
    }
    if ((instruction.opcode() == "cast" || instruction.opcode() == "bitcast") &&
        !instruction.operands().empty()) {
      current = instruction.operands().back();
      continue;
    }
    return std::nullopt;
  }
  return !current.empty() && current.front() == '@'
             ? std::optional<std::string>{current}
             : std::nullopt;
}

FunctionParameterObjects collectParameterObjects(const Module &module) {
  FunctionParameterObjects result;
  std::unordered_map<std::string, const Function *> functions;
  std::unordered_map<std::string, std::size_t> calls;
  for (const auto &function : module.functions()) {
    functions.emplace(function.name(), &function);
    calls.emplace(function.name(), 0);
    auto &parameters = result[function.name()];
    for (const auto &parameter : function.parameters()) {
      parameters.emplace(parameter.name, ParameterObject{});
    }
  }

  for (const auto &caller : module.functions()) {
    const auto definitions = buildDefinitions(caller);
    for (const auto &block : caller.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty()) {
          continue;
        }
        const auto &symbol = instruction.operands().front();
        if (symbol.size() < 2 || symbol.front() != '@')
          continue;
        const auto functionFound = functions.find(symbol.substr(1));
        if (functionFound == functions.end())
          continue;
        const auto &callee = *functionFound->second;
        if (instruction.operands().size() != callee.parameters().size() + 1) {
          continue;
        }
        ++calls[callee.name()];
        auto &parameters = result[callee.name()];
        for (std::size_t index = 0; index < callee.parameters().size();
             ++index) {
          auto &state = parameters[callee.parameters()[index].name];
          if (!state.valid)
            continue;
          const auto object =
              resolveObjectRoot(instruction.operands()[index + 1], definitions);
          if (!object.has_value()) {
            state.valid = false;
          } else if (!state.initialized) {
            state.object = *object;
            state.initialized = true;
          } else if (state.object != *object) {
            state.valid = false;
          }
        }
      }
    }
  }

  for (auto &[function, parameters] : result) {
    if (calls[function] != 0)
      continue;
    for (auto &[name, state] : parameters) {
      (void)name;
      state.valid = false;
    }
  }
  return result;
}

std::optional<std::string>
canonicalObject(const Function &function, const std::string &root,
                const DefinitionMap &definitions,
                const FunctionParameterObjects &parameterObjects) {
  if (!root.empty() && root.front() == '@')
    return root;
  const auto rootDefinition = definitions.find(root);
  if (rootDefinition != definitions.end() &&
      rootDefinition->second.instruction->opcode() == "alloca") {
    return root;
  }
  const auto functionFound = parameterObjects.find(function.name());
  if (functionFound == parameterObjects.end())
    return std::nullopt;
  const auto parameterFound = functionFound->second.find(root);
  if (parameterFound == functionFound->second.end() ||
      !parameterFound->second.valid || !parameterFound->second.initialized) {
    return std::nullopt;
  }
  return parameterFound->second.object;
}

BaseOffset baseOffset(const std::string &value,
                      const DefinitionMap &definitions, unsigned depth = 0) {
  if (depth >= 8)
    return BaseOffset{value, 0};
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return BaseOffset{value, 0};
  const auto &instruction = *found->second.instruction;
  if ((instruction.opcode() != "add" && instruction.opcode() != "sub") ||
      instruction.operands().size() != 2) {
    return BaseOffset{value, 0};
  }
  long long literal = 0;
  if (detail::parseIntegerLiteral(instruction.operands()[1], literal)) {
    auto result = baseOffset(instruction.operands()[0], definitions, depth + 1);
    result.offset += instruction.opcode() == "sub" ? -literal : literal;
    return result;
  }
  if (instruction.opcode() == "add" &&
      detail::parseIntegerLiteral(instruction.operands()[0], literal)) {
    auto result = baseOffset(instruction.operands()[1], definitions, depth + 1);
    result.offset += literal;
    return result;
  }
  return BaseOffset{value, 0};
}

IntegerRange
expressionRange(const std::string &value,
                const std::unordered_map<std::string, IntegerRange> &ranges,
                const DefinitionMap &definitions) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) {
    return IntegerRange{literal, literal, true};
  }
  const auto direct = ranges.find(value);
  if (direct != ranges.end())
    return direct->second;
  const auto offset = baseOffset(value, definitions);
  if (offset.base == value)
    return {};
  const auto base = ranges.find(offset.base);
  if (base == ranges.end() || !base->second.valid)
    return {};
  const auto minimum = base->second.minimum + offset.offset;
  const auto maximum = base->second.maximum + offset.offset;
  if (minimum < std::numeric_limits<std::int32_t>::min() ||
      maximum > std::numeric_limits<std::int32_t>::max()) {
    return {};
  }
  return IntegerRange{minimum, maximum, true};
}

std::unordered_map<std::string, IntegerRange>
computeInductionRanges(const std::vector<LoopInfo> &loops,
                       const DefinitionMap &definitions) {
  std::unordered_map<std::string, IntegerRange> ranges;
  for (std::size_t iteration = 0; iteration < loops.size() * 4 + 4;
       ++iteration) {
    bool changed = false;
    for (const auto &loop : loops) {
      const auto initial = expressionRange(loop.initial, ranges, definitions);
      const auto bound = expressionRange(loop.bound, ranges, definitions);
      if (!initial.valid || !bound.valid)
        continue;
      IntegerRange next;
      if (loop.direction == InductionDirection::Increasing) {
        if (bound.maximum <= std::numeric_limits<std::int32_t>::min())
          continue;
        next = IntegerRange{initial.minimum, bound.maximum - 1, true};
      } else {
        if (bound.minimum <= std::numeric_limits<std::int32_t>::min())
          continue;
        next = IntegerRange{bound.minimum, initial.maximum, true};
      }
      if (next.minimum > next.maximum ||
          next.minimum < std::numeric_limits<std::int32_t>::min() ||
          next.maximum > std::numeric_limits<std::int32_t>::max()) {
        continue;
      }
      const auto old = ranges.find(loop.induction);
      if (old == ranges.end() || !old->second.valid ||
          old->second.minimum != next.minimum ||
          old->second.maximum != next.maximum) {
        ranges[loop.induction] = next;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
  return ranges;
}

bool proveUnequal(const std::string &lhs, const std::string &rhs,
                  const std::vector<LoopInfo> &loops,
                  const std::unordered_map<std::string, IntegerRange> &ranges,
                  const DefinitionMap &definitions) {
  if (lhs == rhs)
    return false;
  long long lhsLiteral = 0;
  long long rhsLiteral = 0;
  if (detail::parseIntegerLiteral(lhs, lhsLiteral) &&
      detail::parseIntegerLiteral(rhs, rhsLiteral)) {
    return lhsLiteral != rhsLiteral;
  }
  const auto lhsOffset = baseOffset(lhs, definitions);
  const auto rhsOffset = baseOffset(rhs, definitions);
  if (lhsOffset.base == rhsOffset.base) {
    const auto difference = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(lhsOffset.offset - rhsOffset.offset));
    return difference != 0;
  }

  for (const auto &loop : loops) {
    if ((lhs == loop.induction && rhs == loop.bound) ||
        (rhs == loop.induction && lhs == loop.bound)) {
      return true;
    }
    if (loop.direction != InductionDirection::Increasing)
      continue;
    const auto initial = baseOffset(loop.initial, definitions);
    const auto provesAfterInitial = [&](const BaseOffset &varying,
                                        const BaseOffset &fixed,
                                        const std::string &varyingValue) {
      if (varying.base != loop.induction || fixed.base != initial.base) {
        return false;
      }
      const auto varyingRange =
          expressionRange(varyingValue, ranges, definitions);
      if (!varyingRange.valid)
        return false;
      return initial.offset + varying.offset > fixed.offset;
    };
    if (provesAfterInitial(lhsOffset, rhsOffset, lhs) ||
        provesAfterInitial(rhsOffset, lhsOffset, rhs)) {
      return true;
    }
  }
  return false;
}

bool proveNoAlias(const Function &function, const AccessPath &lhs,
                  const AccessPath &rhs, const std::vector<LoopInfo> &loops,
                  const std::unordered_map<std::string, IntegerRange> &ranges,
                  const DefinitionMap &definitions,
                  const FunctionParameterObjects &parameterObjects) {
  const auto lhsObject =
      canonicalObject(function, lhs.root, definitions, parameterObjects);
  const auto rhsObject =
      canonicalObject(function, rhs.root, definitions, parameterObjects);
  if (lhsObject.has_value() && rhsObject.has_value() &&
      *lhsObject != *rhsObject) {
    return true;
  }
  if (lhs.root != rhs.root || lhs.indices.size() != rhs.indices.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.indices.size(); ++index) {
    if (proveUnequal(lhs.indices[index], rhs.indices[index], loops, ranges,
                     definitions)) {
      return true;
    }
  }
  return false;
}

bool isAvailableAtIterationEntry(const std::string &value, const LoopInfo &loop,
                                 const DefinitionMap &definitions) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal) || value == loop.induction ||
      (!value.empty() && value.front() == '@')) {
    return true;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return true;
  return loop.region.find(found->second.block) == loop.region.end();
}

bool pathIsIterationInvariant(const AccessPath &path, const LoopInfo &loop,
                              const DefinitionMap &definitions) {
  for (const auto &index : path.indices) {
    if (!isAvailableAtIterationEntry(index, loop, definitions))
      return false;
  }
  return true;
}

std::optional<bool>
constantCondition(const std::string &condition,
                  const std::unordered_map<std::string, IntegerRange> &ranges,
                  const DefinitionMap &definitions) {
  const auto found = definitions.find(condition);
  if (found == definitions.end() ||
      found->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto &instruction = *found->second.instruction;
  const auto lhs =
      expressionRange(instruction.operands()[0], ranges, definitions);
  const auto rhs =
      expressionRange(instruction.operands()[1], ranges, definitions);
  if (!lhs.valid || !rhs.valid)
    return std::nullopt;
  if (instruction.opcode() == "icmp.slt") {
    if (lhs.maximum < rhs.minimum)
      return true;
    if (lhs.minimum >= rhs.maximum)
      return false;
  } else if (instruction.opcode() == "icmp.sle") {
    if (lhs.maximum <= rhs.minimum)
      return true;
    if (lhs.minimum > rhs.maximum)
      return false;
  } else if (instruction.opcode() == "icmp.sgt") {
    if (lhs.minimum > rhs.maximum)
      return true;
    if (lhs.maximum <= rhs.minimum)
      return false;
  } else if (instruction.opcode() == "icmp.sge") {
    if (lhs.minimum >= rhs.maximum)
      return true;
    if (lhs.maximum < rhs.minimum)
      return false;
  } else if (instruction.opcode() == "icmp.eq") {
    if (lhs.minimum == lhs.maximum && rhs.minimum == rhs.maximum &&
        lhs.minimum == rhs.minimum) {
      return true;
    }
    if (lhs.maximum < rhs.minimum || rhs.maximum < lhs.minimum)
      return false;
  } else if (instruction.opcode() == "icmp.ne") {
    if (lhs.maximum < rhs.minimum || rhs.maximum < lhs.minimum)
      return true;
    if (lhs.minimum == lhs.maximum && rhs.minimum == rhs.maximum &&
        lhs.minimum == rhs.minimum) {
      return false;
    }
  }
  return std::nullopt;
}

bool definitelyLoadsPerIteration(
    const Function &function, const CFG &cfg, const Candidate &candidate,
    const std::unordered_map<std::string, IntegerRange> &ranges,
    const DefinitionMap &definitions) {
  std::unordered_set<std::string> loadBlocks;
  for (const auto &blockName : candidate.loop.region) {
    const auto *block = findBlock(function, cfg, blockName);
    if (block == nullptr)
      return false;
    for (const auto &instruction : block->instructions()) {
      if (instruction.opcode() != "load" ||
          instruction.operands().size() != 2) {
        continue;
      }
      const auto path = accessPath(instruction.operands()[1], definitions);
      if (path.has_value() && samePath(*path, candidate.path)) {
        loadBlocks.insert(blockName);
      }
    }
  }
  if (loadBlocks.empty())
    return false;

  std::unordered_map<std::string, unsigned> color;
  const std::unordered_set<std::string> backedges(
      candidate.loop.backedges.begin(), candidate.loop.backedges.end());
  std::function<bool(const std::string &)> unsafePath =
      [&](const std::string &blockName) {
        if (loadBlocks.find(blockName) != loadBlocks.end())
          return false;
        if (backedges.find(blockName) != backedges.end())
          return true;
        const auto state = color[blockName];
        if (state == 1)
          return true;
        if (state == 2)
          return false;
        color[blockName] = 1;
        const auto found = cfg.index.find(blockName);
        if (found == cfg.index.end())
          return true;
        std::vector<std::string> successors = cfg.successors[found->second];
        const auto *block = findBlock(function, cfg, blockName);
        if (block != nullptr && block->terminator() != nullptr &&
            block->terminator()->operands().size() == 3) {
          if (const auto condition = constantCondition(
                  block->terminator()->operands()[0], ranges, definitions)) {
            successors = {
                block->terminator()->operands()[*condition ? 1U : 2U]};
          }
        }
        for (const auto &successor : successors) {
          if (successor == candidate.loop.header)
            return true;
          if (candidate.loop.region.find(successor) !=
                  candidate.loop.region.end() &&
              unsafePath(successor)) {
            return true;
          }
        }
        color[blockName] = 2;
        return false;
      };
  return !unsafePath(candidate.loop.body);
}

std::optional<Candidate>
findCandidate(const Function &function,
              const FunctionParameterObjects &parameterObjects) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto loops = collectLoops(function, cfg, definitions);
  const auto ranges = computeInductionRanges(loops, definitions);
  std::optional<Candidate> best;

  for (const auto &loop : loops) {
    if (loop.direction != InductionDirection::Increasing ||
        loop.region.size() < 4) {
      continue;
    }
    struct Group {
      AccessPath path;
      std::string type;
      std::string address;
      std::size_t loads = 0;
      std::size_t stores = 0;
    };
    std::unordered_map<std::string, Group> groups;
    bool hasCall = false;
    for (const auto &blockName : loop.region) {
      const auto *block = findBlock(function, cfg, blockName);
      if (block == nullptr) {
        hasCall = true;
        break;
      }
      for (const auto &instruction : block->instructions()) {
        if (instruction.opcode() == "call") {
          hasCall = true;
          break;
        }
        if ((instruction.opcode() != "load" &&
             instruction.opcode() != "store") ||
            (instruction.operands().size() != 2 &&
             instruction.operands().size() != 3) ||
            instruction.operands()[0] != "int") {
          continue;
        }
        const auto &address = instruction.opcode() == "load"
                                  ? instruction.operands()[1]
                                  : instruction.operands()[2];
        const auto path = accessPath(address, definitions);
        if (!path.has_value() || path->indices.empty() ||
            !pathIsIterationInvariant(*path, loop, definitions)) {
          continue;
        }
        auto &group = groups[pathKey(*path, instruction.operands()[0])];
        group.path = *path;
        group.type = instruction.operands()[0];
        group.address = address;
        if (instruction.opcode() == "load") {
          ++group.loads;
        } else {
          ++group.stores;
        }
      }
      if (hasCall)
        break;
    }
    if (hasCall)
      continue;

    for (const auto &[key, group] : groups) {
      (void)key;
      if (group.loads < 3 || group.stores == 0 ||
          group.loads + group.stores < 6) {
        continue;
      }
      Candidate candidate{loop,          group.path,  group.type,
                          group.address, group.loads, group.stores};
      bool safe = true;
      for (const auto &blockName : loop.region) {
        const auto *block = findBlock(function, cfg, blockName);
        for (const auto &instruction : block->instructions()) {
          if ((instruction.opcode() != "load" &&
               instruction.opcode() != "store") ||
              (instruction.operands().size() != 2 &&
               instruction.operands().size() != 3)) {
            continue;
          }
          const auto &address = instruction.opcode() == "load"
                                    ? instruction.operands()[1]
                                    : instruction.operands()[2];
          const auto other = accessPath(address, definitions);
          if (!other.has_value()) {
            safe = false;
            break;
          }
          if (samePath(candidate.path, *other)) {
            if (instruction.operands()[0] != candidate.type) {
              safe = false;
              break;
            }
          } else if (!proveNoAlias(function, candidate.path, *other, loops,
                                   ranges, definitions, parameterObjects)) {
            safe = false;
            break;
          }
        }
        if (!safe)
          break;
      }
      if (!safe || !definitelyLoadsPerIteration(function, cfg, candidate,
                                                ranges, definitions)) {
        continue;
      }
      const auto saving = candidate.loads + candidate.stores - 2;
      const auto bestSaving =
          best.has_value() ? best->loads + best->stores - 2 : 0;
      if (!best.has_value() || saving > bestSaving)
        best = candidate;
    }
  }
  return best;
}

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem, bool value) {
  std::string candidate = value ? "%" + stem : stem;
  std::size_t suffix = 0;
  while (!names.insert(candidate).second) {
    candidate = (value ? "%" : "") + stem + "." + std::to_string(++suffix);
  }
  return candidate;
}

bool transform(Function &function, const Candidate &candidate) {
  const auto definitions = buildDefinitions(function);
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        names.insert(instruction.result());
    }
  }
  const auto slot = freshName(names, "iteration.scalar.slot", true);
  const auto initial = freshName(names, "iteration.scalar.initial", true);

  std::unordered_set<std::string> targetAddresses;
  for (const auto &blockName : candidate.loop.region) {
    const auto *block = findBlock(function, blockName);
    if (block == nullptr)
      return false;
    for (const auto &instruction : block->instructions()) {
      if ((instruction.opcode() != "load" && instruction.opcode() != "store") ||
          (instruction.operands().size() != 2 &&
           instruction.operands().size() != 3) ||
          instruction.operands()[0] != candidate.type) {
        continue;
      }
      const auto &address = instruction.opcode() == "load"
                                ? instruction.operands()[1]
                                : instruction.operands()[2];
      const auto path = accessPath(address, definitions);
      if (path.has_value() && samePath(*path, candidate.path)) {
        targetAddresses.insert(address);
      }
    }
  }

  std::unordered_map<std::string, std::string> cloned;
  std::vector<Instruction> addressInstructions;
  std::function<std::optional<std::string>(const std::string &)> cloneAddress =
      [&](const std::string &value) -> std::optional<std::string> {
    const auto already = cloned.find(value);
    if (already != cloned.end())
      return already->second;
    const auto found = definitions.find(value);
    if (found == definitions.end() ||
        candidate.loop.region.find(found->second.block) ==
            candidate.loop.region.end()) {
      return value;
    }
    const auto &instruction = *found->second.instruction;
    if ((instruction.opcode() != "getelementptr" &&
         instruction.opcode() != "getelementptr.linear" &&
         instruction.opcode() != "gep" &&
         instruction.opcode() != "gep.linear") ||
        instruction.operands().size() != 2) {
      return std::nullopt;
    }
    const auto base = cloneAddress(instruction.operands()[0]);
    if (!base.has_value())
      return std::nullopt;
    const auto result = freshName(names, "iteration.scalar.address", true);
    addressInstructions.emplace_back(
        instruction.opcode(),
        std::vector<std::string>{*base, instruction.operands()[1]}, result);
    cloned.emplace(value, result);
    return result;
  };
  const auto address = cloneAddress(candidate.representativeAddress);
  if (!address.has_value())
    return false;

  auto &entry = function.blocks().front();
  auto allocaInsert = entry.instructions().begin();
  while (allocaInsert != entry.instructions().end() &&
         allocaInsert->opcode() == "alloca") {
    ++allocaInsert;
  }
  entry.instructions().insert(allocaInsert,
                              Instruction{"alloca", {candidate.type}, slot});

  for (const auto &blockName : candidate.loop.region) {
    auto *block = findBlock(function, blockName);
    if (block == nullptr)
      return false;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block->instructions().size());
    for (const auto &instruction : block->instructions()) {
      if ((instruction.opcode() == "load" &&
           instruction.operands().size() == 2) ||
          (instruction.opcode() == "store" &&
           instruction.operands().size() == 3)) {
        const auto address = instruction.opcode() == "load"
                                 ? instruction.operands()[1]
                                 : instruction.operands()[2];
        if (targetAddresses.find(address) != targetAddresses.end()) {
          auto operands = instruction.operands();
          operands.back() = slot;
          rewritten.emplace_back(instruction.opcode(), std::move(operands),
                                 instruction.result());
          continue;
        }
      }
      rewritten.push_back(instruction);
    }
    block->instructions() = std::move(rewritten);
  }

  auto *body = findBlock(function, candidate.loop.body);
  if (body == nullptr || body->terminator() == nullptr)
    return false;

  auto bodyInsert = body->instructions().begin();
  while (bodyInsert != body->instructions().end() &&
         bodyInsert->opcode() == "phi") {
    ++bodyInsert;
  }
  bodyInsert = body->instructions().insert(
      bodyInsert, addressInstructions.begin(), addressInstructions.end());
  bodyInsert += static_cast<std::ptrdiff_t>(addressInstructions.size());
  bodyInsert = body->instructions().insert(
      bodyInsert, Instruction{"load", {candidate.type, *address}, initial});
  ++bodyInsert;
  body->instructions().insert(
      bodyInsert, Instruction{"store", {candidate.type, initial, slot}});

  for (const auto &backedgeName : candidate.loop.backedges) {
    auto *backedge = findBlock(function, backedgeName);
    if (backedge == nullptr || backedge->terminator() == nullptr)
      return false;
    const auto final = freshName(names, "iteration.scalar.final", true);
    auto insert = backedge->instructions().end() - 1;
    insert = backedge->instructions().insert(
        insert, Instruction{"load", {candidate.type, slot}, final});
    ++insert;
    backedge->instructions().insert(
        insert, Instruction{"store", {candidate.type, final, *address}});
  }
  return true;
}

} // namespace

void IterationScalarResidencyPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_ITERATION_SCALAR_RESIDENCY") != nullptr) {
    return;
  }
  const bool trace =
      std::getenv("COMPILER_ITERATION_SCALAR_RESIDENCY_STATS") != nullptr;
  const auto parameterObjects = collectParameterObjects(module);
  for (auto &function : module.functions()) {
    std::size_t transformed = 0;
    while (transformed < 8) {
      const auto candidate = findCandidate(function, parameterObjects);
      if (!candidate.has_value() || !transform(function, *candidate))
        break;
      ++transformed;
    }
    if (transformed != 0)
      function.setIterationScalarResidency(true);
    if (trace) {
      std::cerr << "iteration-scalar-residency: " << function.name()
                << ": transformed=" << transformed << '\n';
    }
  }
}

} // namespace compiler::midend::pass
