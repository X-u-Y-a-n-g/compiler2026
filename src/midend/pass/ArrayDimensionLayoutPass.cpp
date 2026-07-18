#include "../../../include/compiler/midend/pass/ArrayDimensionLayoutPass.h"

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
using compiler::ir::core::Constant;
using compiler::ir::core::Function;
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr long double kUnknownTripFactor = 16.0L;
constexpr long double kWeightLimit = 1.0e18L;
constexpr long double kRequiredBenefitRatio = 2.0L;
constexpr std::size_t kMaxMaterializedInitializerElements = 1U << 20;

struct Location {
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Definition {
  const Instruction* instruction = nullptr;
  Location location;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct ArrayType {
  std::string scalar;
  std::size_t rows = 0;
  std::size_t columns = 0;
};

struct PointerPath {
  std::string base;
  std::vector<std::string> indices;
};

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<bool> reachable;
  std::vector<std::unordered_set<std::size_t>> dominators;
};

struct LoopInfo {
  std::size_t header = 0;
  std::vector<std::size_t> latches;
  std::unordered_set<std::size_t> blocks;
  std::string induction;
  long double tripFactor = kUnknownTripFactor;
};

struct FunctionAnalysis {
  DefinitionMap definitions;
  CFG cfg;
  std::vector<LoopInfo> loops;
  std::unordered_map<std::string, std::vector<std::size_t>> inductionLoops;
  std::vector<long double> blockWeights;
};

struct PointsTo {
  std::unordered_set<std::string> globals;
  bool unknown = false;
};

struct CallSite {
  std::size_t caller = 0;
  std::size_t callee = 0;
  std::size_t block = 0;
  const Instruction* instruction = nullptr;
  long double multiplier = 1.0L;
};

struct Access {
  std::size_t function = 0;
  Location location;
  std::string root;
  std::vector<std::string> indices;
};

struct Usage {
  bool safe = true;
  std::vector<Access> accesses;
  long double benefit = 0.0L;
  long double harm = 0.0L;
};

bool isGepOpcode(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "gep";
}

long double multiplyWeight(long double lhs, long double rhs) {
  if (lhs <= 0.0L || rhs <= 0.0L) {
    return 0.0L;
  }
  if (lhs >= kWeightLimit / rhs) {
    return kWeightLimit;
  }
  return std::min(kWeightLimit, lhs * rhs);
}

std::optional<ArrayType> parseTwoDimensionalArray(const std::string& text) {
  const auto firstOpen = text.find('[');
  if (firstOpen == std::string::npos) {
    return std::nullopt;
  }
  const auto firstClose = text.find(']', firstOpen + 1);
  const auto secondOpen =
      firstClose == std::string::npos ? std::string::npos
                                     : text.find('[', firstClose + 1);
  const auto secondClose =
      secondOpen == std::string::npos ? std::string::npos
                                     : text.find(']', secondOpen + 1);
  if (firstClose == std::string::npos || secondOpen == std::string::npos ||
      secondClose == std::string::npos ||
      text.find('[', secondClose + 1) != std::string::npos) {
    return std::nullopt;
  }
  long long rows = 0;
  long long columns = 0;
  if (!detail::parseIntegerLiteral(
          text.substr(firstOpen + 1, firstClose - firstOpen - 1), rows) ||
      !detail::parseIntegerLiteral(
          text.substr(secondOpen + 1, secondClose - secondOpen - 1), columns) ||
      rows <= 0 || columns <= 0) {
    return std::nullopt;
  }
  return ArrayType{text.substr(0, firstOpen),
                   static_cast<std::size_t>(rows),
                   static_cast<std::size_t>(columns)};
}

std::string arrayTypeText(const ArrayType& type) {
  return type.scalar + "[" + std::to_string(type.rows) + "][" +
         std::to_string(type.columns) + "]";
}

std::string parameterTypeText(const ArrayType& physicalType) {
  return physicalType.scalar + "[][" +
         std::to_string(physicalType.columns) + "]";
}

std::unordered_map<std::string, ArrayType> collectArrayTypes(
    const Module& module) {
  std::unordered_map<std::string, ArrayType> result;
  for (const auto& global : module.globals()) {
    const auto parsed = parseTwoDimensionalArray(global.type());
    if (parsed.has_value()) {
      result.emplace("@" + global.name(), *parsed);
    }
  }
  return result;
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].hasResult()) {
        definitions.emplace(instructions[index].result(),
                            Definition{&instructions[index],
                                       Location{block, index}});
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
    return PointerPath{value, {}};
  }
  const auto& instruction = *definition->second.instruction;
  if (instruction.opcode() == "alloca") {
    return PointerPath{value, {}};
  }
  if (!isGepOpcode(instruction.opcode()) ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }
  auto base = pointerPath(instruction.operands()[0], definitions, visiting);
  if (!base.has_value()) {
    return std::nullopt;
  }
  base->indices.push_back(instruction.operands()[1]);
  return base;
}

std::optional<PointerPath> pointerPath(const std::string& value,
                                       const DefinitionMap& definitions) {
  std::unordered_set<std::string> visiting;
  return pointerPath(value, definitions, visiting);
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
  const auto count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.reachable.assign(count, false);
  cfg.dominators.resize(count);
  for (std::size_t block = 0; block < count; ++block) {
    cfg.index.emplace(function.blocks()[block].name(), block);
  }
  for (std::size_t block = 0; block < count; ++block) {
    for (const auto& target : branchTargets(function.blocks()[block])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end()) {
        continue;
      }
      cfg.successors[block].push_back(found->second);
      cfg.predecessors[found->second].push_back(block);
    }
  }
  if (count == 0) {
    return cfg;
  }

  std::deque<std::size_t> worklist;
  cfg.reachable[0] = true;
  worklist.push_back(0);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto successor : cfg.successors[block]) {
      if (!cfg.reachable[successor]) {
        cfg.reachable[successor] = true;
        worklist.push_back(successor);
      }
    }
  }

  std::unordered_set<std::size_t> allReachable;
  for (std::size_t block = 0; block < count; ++block) {
    if (cfg.reachable[block]) {
      allReachable.insert(block);
    }
  }
  cfg.dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    if (cfg.reachable[block]) {
      cfg.dominators[block] = allReachable;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      if (!cfg.reachable[block]) {
        continue;
      }
      std::optional<std::unordered_set<std::size_t>> intersection;
      for (const auto predecessor : cfg.predecessors[block]) {
        if (!cfg.reachable[predecessor]) {
          continue;
        }
        if (!intersection.has_value()) {
          intersection = cfg.dominators[predecessor];
          continue;
        }
        for (auto iterator = intersection->begin();
             iterator != intersection->end();) {
          if (cfg.dominators[predecessor].find(*iterator) ==
              cfg.dominators[predecessor].end()) {
            iterator = intersection->erase(iterator);
          } else {
            ++iterator;
          }
        }
      }
      auto next = intersection.value_or(std::unordered_set<std::size_t>{});
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
         cfg.dominators[block].find(dominator) !=
             cfg.dominators[block].end();
}

std::vector<LoopInfo> findNaturalLoops(const CFG& cfg) {
  std::unordered_map<std::size_t, LoopInfo> byHeader;
  for (std::size_t latch = 0; latch < cfg.successors.size(); ++latch) {
    if (!cfg.reachable[latch]) {
      continue;
    }
    for (const auto header : cfg.successors[latch]) {
      if (!dominates(cfg, header, latch)) {
        continue;
      }
      std::unordered_set<std::size_t> blocks{header, latch};
      std::deque<std::size_t> worklist;
      if (latch != header) {
        worklist.push_back(latch);
      }
      while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop_front();
        for (const auto predecessor : cfg.predecessors[block]) {
          if (blocks.insert(predecessor).second && predecessor != header) {
            worklist.push_back(predecessor);
          }
        }
      }
      auto& loop = byHeader[header];
      loop.header = header;
      loop.blocks.insert(blocks.begin(), blocks.end());
      if (std::find(loop.latches.begin(), loop.latches.end(), latch) ==
          loop.latches.end()) {
        loop.latches.push_back(latch);
      }
    }
  }
  std::vector<LoopInfo> loops;
  for (auto& [header, loop] : byHeader) {
    std::sort(loop.latches.begin(), loop.latches.end());
    loops.push_back(std::move(loop));
  }
  std::sort(loops.begin(), loops.end(), [](const LoopInfo& lhs,
                                           const LoopInfo& rhs) {
    return lhs.blocks.size() < rhs.blocks.size();
  });
  return loops;
}

bool isUnitIncrement(const std::string& next, const std::string& induction,
                     std::size_t latch, const DefinitionMap& definitions) {
  const auto found = definitions.find(next);
  if (found == definitions.end() || found->second.location.block != latch ||
      found->second.instruction->opcode() != "add" ||
      found->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto& operands = found->second.instruction->operands();
  long long step = 0;
  return (operands[0] == induction &&
          detail::parseIntegerLiteral(operands[1], step) && step == 1) ||
         (operands[1] == induction &&
          detail::parseIntegerLiteral(operands[0], step) && step == 1);
}

void identifyInduction(FunctionAnalysis& analysis, const Function& function,
                       LoopInfo& loop) {
  const auto& header = function.blocks()[loop.header];
  const auto* terminator = header.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3) {
    return;
  }
  const auto condition = analysis.definitions.find(terminator->operands()[0]);
  if (condition == analysis.definitions.end() ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2) {
    return;
  }
  const auto induction = condition->second.instruction->operands()[0];
  const auto phi = analysis.definitions.find(induction);
  if (phi == analysis.definitions.end() ||
      phi->second.location.block != loop.header ||
      phi->second.instruction->opcode() != "phi") {
    return;
  }
  const auto& operands = phi->second.instruction->operands();
  bool hasZeroEntry = false;
  std::unordered_set<std::size_t> matchedLatches;
  for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
    const auto predecessor = analysis.cfg.index.find(operands[index + 1]);
    if (predecessor == analysis.cfg.index.end()) {
      return;
    }
    if (loop.blocks.find(predecessor->second) == loop.blocks.end()) {
      long long initial = 0;
      if (hasZeroEntry || !detail::parseIntegerLiteral(operands[index], initial) ||
          initial != 0) {
        return;
      }
      hasZeroEntry = true;
      continue;
    }
    if (std::find(loop.latches.begin(), loop.latches.end(),
                  predecessor->second) == loop.latches.end() ||
        !isUnitIncrement(operands[index], induction, predecessor->second,
                         analysis.definitions)) {
      return;
    }
    matchedLatches.insert(predecessor->second);
  }
  if (!hasZeroEntry || matchedLatches.size() != loop.latches.size()) {
    return;
  }
  loop.induction = induction;
  long long bound = 0;
  if (detail::parseIntegerLiteral(
          condition->second.instruction->operands()[1], bound) &&
      bound > 0) {
    loop.tripFactor = std::min<long double>(
        static_cast<long double>(bound), 4096.0L);
  }
}

FunctionAnalysis analyzeFunction(const Function& function) {
  FunctionAnalysis analysis;
  analysis.definitions = buildDefinitions(function);
  analysis.cfg = buildCFG(function);
  analysis.loops = findNaturalLoops(analysis.cfg);
  for (std::size_t index = 0; index < analysis.loops.size(); ++index) {
    identifyInduction(analysis, function, analysis.loops[index]);
    if (!analysis.loops[index].induction.empty()) {
      analysis.inductionLoops[analysis.loops[index].induction].push_back(index);
    }
  }
  analysis.blockWeights.assign(function.blocks().size(), 1.0L);
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& loop : analysis.loops) {
      if (loop.blocks.find(block) != loop.blocks.end()) {
        analysis.blockWeights[block] =
            multiplyWeight(analysis.blockWeights[block], loop.tripFactor);
      }
    }
  }
  return analysis;
}

std::string calledFunction(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& callee = instruction.operands()[0];
  return callee.size() > 1 && callee.front() == '@' ? callee.substr(1)
                                                    : std::string{};
}

bool isArrayParameter(const compiler::ir::core::FunctionParameter& parameter) {
  return parameter.type.find('[') != std::string::npos;
}

std::vector<CallSite> collectCallSites(
    const Module& module, const std::vector<FunctionAnalysis>& analyses,
    const std::unordered_map<std::string, std::size_t>& functions) {
  std::vector<CallSite> calls;
  for (std::size_t caller = 0; caller < module.functions().size(); ++caller) {
    const auto& function = module.functions()[caller];
    for (std::size_t block = 0; block < function.blocks().size(); ++block) {
      for (const auto& instruction : function.blocks()[block].instructions()) {
        const auto calleeName = calledFunction(instruction);
        const auto callee = functions.find(calleeName);
        if (callee == functions.end()) {
          continue;
        }
        calls.push_back(CallSite{caller, callee->second, block, &instruction,
                                 analyses[caller].blockWeights[block]});
      }
    }
  }
  return calls;
}

std::vector<long double> computeFunctionWeights(
    const Module& module, const std::vector<CallSite>& calls,
    const std::unordered_map<std::string, std::size_t>& functions) {
  const auto count = module.functions().size();
  std::vector<std::unordered_set<std::size_t>> edges(count);
  std::vector<std::size_t> indegree(count, 0);
  for (const auto& call : calls) {
    if (edges[call.caller].insert(call.callee).second) {
      ++indegree[call.callee];
    }
  }
  std::vector<std::vector<const CallSite*>> callsByCaller(count);
  for (const auto& call : calls) {
    callsByCaller[call.caller].push_back(&call);
  }
  std::vector<long double> weights(count, 0.0L);
  const auto main = functions.find("main");
  if (main != functions.end()) {
    weights[main->second] = 1.0L;
  } else {
    for (std::size_t function = 0; function < count; ++function) {
      if (indegree[function] == 0) {
        weights[function] = 1.0L;
      }
    }
  }
  std::deque<std::size_t> worklist;
  for (std::size_t function = 0; function < count; ++function) {
    if (indegree[function] == 0) {
      worklist.push_back(function);
    }
  }
  while (!worklist.empty()) {
    const auto caller = worklist.front();
    worklist.pop_front();
    for (const auto* call : callsByCaller[caller]) {
      weights[call->callee] = std::min(
          kWeightLimit,
          weights[call->callee] +
              multiplyWeight(weights[caller], call->multiplier));
    }
    for (const auto callee : edges[caller]) {
      if (--indegree[callee] == 0) {
        worklist.push_back(callee);
      }
    }
  }
  return weights;
}

bool mergePoints(PointsTo& destination, const PointsTo& source) {
  bool changed = false;
  if (source.unknown && !destination.unknown) {
    destination.unknown = true;
    changed = true;
  }
  for (const auto& global : source.globals) {
    changed |= destination.globals.insert(global).second;
  }
  return changed;
}

std::optional<std::size_t> parameterIndex(const Function& function,
                                          const std::string& value) {
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    if (function.parameters()[index].name == value) {
      return index;
    }
  }
  return std::nullopt;
}

std::vector<std::vector<PointsTo>> computePointsTo(
    const Module& module, const std::vector<CallSite>& calls,
    const std::unordered_map<std::string, ArrayType>& arrays) {
  std::vector<std::vector<PointsTo>> points(module.functions().size());
  for (std::size_t function = 0; function < module.functions().size(); ++function) {
    points[function].resize(module.functions()[function].parameters().size());
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& call : calls) {
      const auto& caller = module.functions()[call.caller];
      const auto& callee = module.functions()[call.callee];
      const auto& operands = call.instruction->operands();
      for (std::size_t parameter = 0; parameter < callee.parameters().size();
           ++parameter) {
        if (!isArrayParameter(callee.parameters()[parameter]) ||
            parameter + 1 >= operands.size()) {
          continue;
        }
        PointsTo incoming;
        const auto& actual = operands[parameter + 1];
        if (arrays.find(actual) != arrays.end()) {
          incoming.globals.insert(actual);
        } else if (const auto callerParameter = parameterIndex(caller, actual);
                   callerParameter.has_value() &&
                   isArrayParameter(caller.parameters()[*callerParameter])) {
          incoming = points[call.caller][*callerParameter];
        } else {
          incoming.unknown = true;
        }
        changed |= mergePoints(points[call.callee][parameter], incoming);
      }
    }
  }
  return points;
}

std::optional<std::string> rootGlobal(
    const Module& module, std::size_t function, const std::string& root,
    const std::unordered_map<std::string, ArrayType>& arrays,
    const std::vector<std::vector<PointsTo>>& points) {
  if (arrays.find(root) != arrays.end()) {
    return root;
  }
  const auto parameter = parameterIndex(module.functions()[function], root);
  if (!parameter.has_value()) {
    return std::nullopt;
  }
  const auto& target = points[function][*parameter];
  if (target.unknown || target.globals.size() != 1) {
    return std::nullopt;
  }
  return *target.globals.begin();
}

bool calleeParameterMatches(
    const std::vector<std::vector<PointsTo>>& points, std::size_t callee,
    std::size_t parameter, const std::string& global) {
  if (callee >= points.size() || parameter >= points[callee].size()) {
    return false;
  }
  const auto& target = points[callee][parameter];
  return !target.unknown && target.globals.size() == 1 &&
         *target.globals.begin() == global;
}

void collectUsages(
    const Module& module, const std::vector<FunctionAnalysis>& analyses,
    const std::unordered_map<std::string, std::size_t>& functions,
    const std::unordered_map<std::string, ArrayType>& arrays,
    const std::vector<std::vector<PointsTo>>& points,
    std::unordered_map<std::string, Usage>& usages) {
  for (std::size_t function = 0; function < module.functions().size(); ++function) {
    for (const auto& target : points[function]) {
      if (target.unknown || target.globals.size() != 1) {
        for (const auto& global : target.globals) {
          usages[global].safe = false;
        }
      }
    }

    const auto& currentFunction = module.functions()[function];
    const auto& definitions = analyses[function].definitions;
    std::unordered_set<std::string> seenAccesses;
    for (std::size_t block = 0; block < currentFunction.blocks().size(); ++block) {
      const auto& instructions = currentFunction.blocks()[block].instructions();
      for (std::size_t instructionIndex = 0;
           instructionIndex < instructions.size(); ++instructionIndex) {
        const auto& instruction = instructions[instructionIndex];
        for (std::size_t operandIndex = 0;
             operandIndex < instruction.operands().size(); ++operandIndex) {
          const auto path = pointerPath(instruction.operands()[operandIndex],
                                        definitions);
          if (!path.has_value()) {
            continue;
          }
          const auto global = rootGlobal(module, function, path->base, arrays,
                                         points);
          if (!global.has_value()) {
            continue;
          }
          auto& usage = usages[*global];
          if (isGepOpcode(instruction.opcode()) && operandIndex == 0) {
            continue;
          }
          const bool isLoad = instruction.opcode() == "load" && operandIndex == 1;
          const bool isStore = instruction.opcode() == "store" && operandIndex == 2;
          if (isLoad || isStore) {
            if (path->indices.size() != 2) {
              usage.safe = false;
              continue;
            }
            const std::string key = std::to_string(function) + ":" +
                                    std::to_string(block) + ":" +
                                    std::to_string(instructionIndex);
            if (seenAccesses.insert(key).second) {
              usage.accesses.push_back(
                  Access{function, Location{block, instructionIndex},
                         path->base, path->indices});
            }
            continue;
          }
          if (instruction.opcode() == "call" && operandIndex > 0 &&
              path->indices.empty()) {
            const auto callee = functions.find(calledFunction(instruction));
            const auto parameter = operandIndex - 1;
            if (callee != functions.end() &&
                calleeParameterMatches(points, callee->second, parameter,
                                       *global)) {
              continue;
            }
          }
          usage.safe = false;
        }
      }
    }
  }
}

std::optional<std::size_t> innermostVaryingDimension(
    const FunctionAnalysis& analysis, const Access& access) {
  std::optional<std::size_t> dimension;
  std::size_t smallestLoop = std::numeric_limits<std::size_t>::max();
  for (std::size_t current = 0; current < access.indices.size(); ++current) {
    const auto found = analysis.inductionLoops.find(access.indices[current]);
    if (found == analysis.inductionLoops.end()) {
      continue;
    }
    for (const auto loopIndex : found->second) {
      const auto& loop = analysis.loops[loopIndex];
      if (loop.blocks.find(access.location.block) == loop.blocks.end() ||
          loop.blocks.size() >= smallestLoop) {
        continue;
      }
      smallestLoop = loop.blocks.size();
      dimension = current;
    }
  }
  return dimension;
}

void scoreUsages(const std::vector<FunctionAnalysis>& analyses,
                 const std::vector<long double>& functionWeights,
                 const std::unordered_map<std::string, ArrayType>& arrays,
                 std::unordered_map<std::string, Usage>& usages) {
  for (auto& [global, usage] : usages) {
    const auto type = arrays.find(global);
    if (type == arrays.end()) {
      usage.safe = false;
      continue;
    }
    for (const auto& access : usage.accesses) {
      const auto& analysis = analyses[access.function];
      const bool allDynamicIndicesUnderstood =
          std::all_of(access.indices.begin(), access.indices.end(),
                      [&](const std::string& index) {
                        long long literal = 0;
                        return detail::parseIntegerLiteral(index, literal) ||
                               analysis.inductionLoops.find(index) !=
                                   analysis.inductionLoops.end();
                      });
      if (!allDynamicIndicesUnderstood) {
        usage.safe = false;
        break;
      }
      const auto dimension =
          innermostVaryingDimension(analysis, access);
      if (!dimension.has_value()) {
        continue;
      }
      const auto weight = multiplyWeight(
          functionWeights[access.function],
          analysis.blockWeights[access.location.block]);
      const long double oldStride =
          *dimension == 0 ? static_cast<long double>(type->second.columns)
                          : 1.0L;
      const long double newStride =
          *dimension == 0 ? 1.0L
                          : static_cast<long double>(type->second.rows);
      const auto delta = (oldStride - newStride) * weight;
      if (delta > 0.0L) {
        usage.benefit += delta;
      } else {
        usage.harm -= delta;
      }
    }
  }
}

bool isZeroConstant(const Constant& constant) {
  if (constant.isAggregate()) {
    return std::all_of(constant.elements().begin(), constant.elements().end(),
                       isZeroConstant);
  }
  long long integer = 0;
  if (detail::parseIntegerLiteral(constant.literal(), integer)) {
    return integer == 0;
  }
  double floating = 0.0;
  return detail::parseFloatLiteral(constant.literal(), floating) &&
         floating == 0.0;
}

Constant zeroScalar(const std::string& scalar) {
  return Constant::scalar(scalar, scalar == "float" ? "0.0" : "0");
}

Constant sourceElement(const Constant& initializer, const ArrayType& type,
                       std::size_t row, std::size_t column) {
  if (!initializer.isAggregate() || row >= initializer.elements().size()) {
    return zeroScalar(type.scalar);
  }
  const auto& sourceRow = initializer.elements()[row];
  if (!sourceRow.isAggregate() || column >= sourceRow.elements().size()) {
    return zeroScalar(type.scalar);
  }
  return sourceRow.elements()[column];
}

Constant transposeInitializer(const Constant& initializer,
                              const ArrayType& oldType) {
  const ArrayType newType{oldType.scalar, oldType.columns, oldType.rows};
  if (isZeroConstant(initializer)) {
    return Constant::aggregate(arrayTypeText(newType), {});
  }
  std::vector<Constant> rows;
  rows.reserve(newType.rows);
  const std::string rowType =
      oldType.scalar + "[" + std::to_string(newType.columns) + "]";
  for (std::size_t row = 0; row < newType.rows; ++row) {
    std::vector<Constant> elements;
    elements.reserve(newType.columns);
    for (std::size_t column = 0; column < newType.columns; ++column) {
      elements.push_back(sourceElement(initializer, oldType, column, row));
    }
    while (!elements.empty() && isZeroConstant(elements.back())) {
      elements.pop_back();
    }
    rows.push_back(Constant::aggregate(rowType, std::move(elements)));
  }
  while (!rows.empty() && isZeroConstant(rows.back())) {
    rows.pop_back();
  }
  return Constant::aggregate(arrayTypeText(newType), std::move(rows));
}

bool initializerCanBeTransposed(const GlobalVariable& global,
                                const ArrayType& type) {
  if (isZeroConstant(global.initializer())) {
    return true;
  }
  return type.rows <= kMaxMaterializedInitializerElements / type.columns;
}

std::string freshValue(std::unordered_set<std::string>& names,
                       std::size_t& counter) {
  while (true) {
    const auto candidate = "%array.layout.gep." + std::to_string(counter++);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

void rewriteAccesses(Module& module,
                     const std::vector<std::vector<PointsTo>>& points,
                     const std::unordered_map<std::string, ArrayType>& arrays,
                     const std::unordered_set<std::string>& selected,
                     const std::unordered_map<std::string, Usage>& usages) {
  struct Rewrite {
    std::string root;
    std::vector<std::string> indices;
  };
  using InstructionRewrites = std::unordered_map<std::size_t, Rewrite>;
  using BlockRewrites = std::unordered_map<std::size_t, InstructionRewrites>;
  std::vector<BlockRewrites> rewrites(
      module.functions().size());
  for (const auto& global : selected) {
    const auto& usage = usages.at(global);
    for (const auto& access : usage.accesses) {
      rewrites[access.function][access.location.block].emplace(
          access.location.instruction,
          Rewrite{access.root, access.indices});
    }
  }

  for (std::size_t functionIndex = 0;
       functionIndex < module.functions().size(); ++functionIndex) {
    auto& function = module.functions()[functionIndex];
    std::unordered_set<std::string> names;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.hasResult()) {
          names.insert(instruction.result());
        }
      }
    }
    std::size_t counter = 0;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      auto& block = function.blocks()[blockIndex];
      std::vector<Instruction> rewritten;
      rewritten.reserve(block.instructions().size() + 8);
      for (std::size_t instructionIndex = 0;
           instructionIndex < block.instructions().size(); ++instructionIndex) {
        const auto& instruction = block.instructions()[instructionIndex];
        const auto blockRewrites = rewrites[functionIndex].find(blockIndex);
        const auto found = blockRewrites == rewrites[functionIndex].end()
                               ? nullptr
                               : &blockRewrites->second;
        const auto rewrite = found == nullptr
                                 ? nullptr
                                 : found->find(instructionIndex) == found->end()
                                       ? nullptr
                                       : &found->at(instructionIndex);
        if (rewrite == nullptr) {
          rewritten.push_back(instruction);
          continue;
        }
        const auto row = freshValue(names, counter);
        const auto element = freshValue(names, counter);
        rewritten.emplace_back(
            "getelementptr",
            std::vector<std::string>{rewrite->root, rewrite->indices[1]},
            row);
        rewritten.emplace_back(
            "getelementptr",
            std::vector<std::string>{row, rewrite->indices[0]}, element);
        auto operands = instruction.operands();
        if (instruction.opcode() == "load") {
          operands[1] = element;
        } else {
          operands[2] = element;
        }
        rewritten.emplace_back(instruction.opcode(), std::move(operands),
                               instruction.result());
      }
      block.instructions() = std::move(rewritten);
    }

    for (std::size_t parameter = 0; parameter < function.parameters().size();
         ++parameter) {
      const auto& target = points[functionIndex][parameter];
      if (target.unknown || target.globals.size() != 1 ||
          selected.find(*target.globals.begin()) == selected.end()) {
        continue;
      }
      const auto oldType = arrays.at(*target.globals.begin());
      const ArrayType physical{oldType.scalar, oldType.columns, oldType.rows};
      function.parameters()[parameter].type = parameterTypeText(physical);
    }
  }
}

void transposeGlobals(Module& module,
                      const std::unordered_map<std::string, ArrayType>& arrays,
                      const std::unordered_set<std::string>& selected) {
  for (auto& global : module.globals()) {
    const auto name = "@" + global.name();
    if (selected.find(name) == selected.end()) {
      continue;
    }
    const auto oldType = arrays.at(name);
    const ArrayType newType{oldType.scalar, oldType.columns, oldType.rows};
    global = GlobalVariable(global.name(), arrayTypeText(newType),
                            transposeInitializer(global.initializer(), oldType),
                            global.isConst());
  }
}

}  // namespace

void ArrayDimensionLayoutPass::run(Module& module) {
  const auto arrays = collectArrayTypes(module);
  if (arrays.empty()) {
    return;
  }
  std::unordered_map<std::string, std::size_t> functions;
  std::vector<FunctionAnalysis> analyses;
  analyses.reserve(module.functions().size());
  for (std::size_t index = 0; index < module.functions().size(); ++index) {
    functions.emplace(module.functions()[index].name(), index);
    analyses.push_back(analyzeFunction(module.functions()[index]));
  }
  const auto calls = collectCallSites(module, analyses, functions);
  const auto functionWeights = computeFunctionWeights(module, calls, functions);
  const auto points = computePointsTo(module, calls, arrays);
  std::unordered_map<std::string, Usage> usages;
  for (const auto& [global, type] : arrays) {
    usages.emplace(global, Usage{});
  }
  collectUsages(module, analyses, functions, arrays, points, usages);
  scoreUsages(analyses, functionWeights, arrays, usages);

  std::unordered_set<std::string> selected;
  for (const auto& global : module.globals()) {
    const auto name = "@" + global.name();
    const auto type = arrays.find(name);
    const auto usage = usages.find(name);
    if (type == arrays.end() || usage == usages.end() || !usage->second.safe ||
        usage->second.accesses.empty() || usage->second.benefit <= 0.0L ||
        usage->second.benefit <
            usage->second.harm * kRequiredBenefitRatio ||
        !initializerCanBeTransposed(global, type->second)) {
      continue;
    }
    selected.insert(name);
  }
  if (selected.empty()) {
    return;
  }
  rewriteAccesses(module, points, arrays, selected, usages);
  transposeGlobals(module, arrays, selected);
}

}  // namespace compiler::midend::pass
