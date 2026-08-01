#include "../../../include/compiler/midend/pass/AssociativeChainAggregationPass.h"

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
using compiler::ir::core::Module;

struct Definition {
  const Instruction* instruction = nullptr;
  std::string block;
  std::size_t index = 0;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct PointerPath {
  std::string root;
  std::vector<std::string> indices;
};

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> successors;
  std::vector<std::vector<std::string>> predecessors;
};

struct CounterUpdate {
  std::string root;
  std::string result;
  std::size_t storeIndex = 0;
};

struct ProducerCandidate {
  std::size_t functionIndex = 0;
  std::string duplicateBlock;
  std::string keyParameter;
  std::string valueParameter;
  std::string counterRoot;
  std::string keyRoot;
  std::string valueRoot;
  std::string nextValueRoot;
  std::string nodeIndex;
  std::string currentNode;
  std::string valueGepOpcode;
  std::size_t counterStoreIndex = 0;
  std::size_t nextValueNewStoreIndex = 0;
  std::size_t nextValueCurrentStoreIndex = 0;
  std::size_t valueStoreIndex = 0;
};

struct ConsumerCandidate {
  std::size_t functionIndex = 0;
  std::string matchBlock;
  std::string innerHeader;
  std::string innerBody;
  std::string innerExit;
  std::string accumulatorSlot;
  std::string nodeSlot;
  std::string matchedNode;
  std::string valueGepOpcode;
  std::size_t accumulatorInitStoreIndex = 0;
  std::size_t nodeInitStoreIndex = 0;
};

struct PairCandidate {
  ProducerCandidate producer;
  ConsumerCandidate consumer;
  std::unordered_set<std::string> relevantRoots;
};

bool isProjection(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "getelementptr.inbounds" ||
         opcode == "getelementptr.linear" || opcode == "gep" ||
         opcode == "gep.linear" || opcode == "cast";
}

bool isGep(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "getelementptr.inbounds" ||
         opcode == "getelementptr.linear" || opcode == "gep" ||
         opcode == "gep.linear";
}

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
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

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.successors.resize(function.blocks().size());
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    for (const auto& target : branchTargets(function.blocks()[index])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end()) {
        continue;
      }
      cfg.successors[index].push_back(target);
      cfg.predecessors[found->second].push_back(
          function.blocks()[index].name());
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (const auto& block : function.blocks()) {
    for (std::size_t index = 0; index < block.instructions().size(); ++index) {
      const auto& instruction = block.instructions()[index];
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name(), index});
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

std::optional<PointerPath> pointerPath(
    const std::string& value, const DefinitionMap& definitions,
    std::unordered_set<std::string>& visiting) {
  if (!value.empty() && value.front() == '@') {
    return PointerPath{value, {}};
  }
  if (value.empty() || value.front() != '%' ||
      !visiting.insert(value).second) {
    return std::nullopt;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end() || found->second.instruction == nullptr) {
    return std::nullopt;
  }
  const auto& instruction = *found->second.instruction;
  if (isGep(instruction.opcode())) {
    if (instruction.operands().size() < 2) {
      return std::nullopt;
    }
    auto path = pointerPath(instruction.operands().front(), definitions,
                            visiting);
    if (!path.has_value()) {
      return std::nullopt;
    }
    for (std::size_t index = 1; index < instruction.operands().size();
         ++index) {
      path->indices.push_back(instruction.operands()[index]);
    }
    return path;
  }
  if (instruction.opcode() == "cast" && instruction.operands().size() == 1) {
    return pointerPath(instruction.operands().front(), definitions, visiting);
  }
  return std::nullopt;
}

std::optional<PointerPath> pointerPath(const std::string& value,
                                       const DefinitionMap& definitions) {
  std::unordered_set<std::string> visiting;
  return pointerPath(value, definitions, visiting);
}

bool isIntegerLiteral(const std::string& value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

bool isI32Load(const Instruction& instruction) {
  return instruction.opcode() == "load" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         instruction.operands()[0] == "int";
}

bool isI32Store(const Instruction& instruction) {
  return instruction.opcode() == "store" &&
         instruction.operands().size() == 3 &&
         instruction.operands()[0] == "int";
}

bool isArrayGlobal(const Module& module, const std::string& root) {
  if (root.size() < 2 || root.front() != '@') {
    return false;
  }
  const auto name = root.substr(1);
  for (const auto& global : module.globals()) {
    if (global.name() == name) {
      return !global.isConst() && global.type().find('[') != std::string::npos &&
             global.type().find("int") != std::string::npos;
    }
  }
  return false;
}

bool isScalarGlobal(const Module& module, const std::string& root) {
  if (root.size() < 2 || root.front() != '@') {
    return false;
  }
  const auto name = root.substr(1);
  for (const auto& global : module.globals()) {
    if (global.name() == name) {
      return !global.isConst() && global.type() == "int";
    }
  }
  return false;
}

bool blockInCycle(const CFG& cfg, const std::string& target) {
  const auto found = cfg.index.find(target);
  if (found == cfg.index.end()) {
    return false;
  }
  std::vector<std::string> worklist = cfg.successors[found->second];
  std::unordered_set<std::string> visited;
  while (!worklist.empty()) {
    const auto current = worklist.back();
    worklist.pop_back();
    if (current == target) {
      return true;
    }
    if (!visited.insert(current).second) {
      continue;
    }
    const auto next = cfg.index.find(current);
    if (next == cfg.index.end()) {
      continue;
    }
    for (const auto& successor : cfg.successors[next->second]) {
      worklist.push_back(successor);
    }
  }
  return false;
}

std::unordered_set<std::string> parameterNames(const Function& function) {
  std::unordered_set<std::string> result;
  for (const auto& parameter : function.parameters()) {
    result.insert(parameter.name);
  }
  return result;
}

bool hasParameter(const Function& function, const std::string& value) {
  for (const auto& parameter : function.parameters()) {
    if (parameter.name == value && parameter.type == "int") {
      return true;
    }
  }
  return false;
}

std::optional<CounterUpdate> findCounterUpdate(
    const Module& module, const BasicBlock& block, const DefinitionMap& definitions) {
  std::optional<CounterUpdate> result;
  for (std::size_t index = 0; index < block.instructions().size(); ++index) {
    const auto& instruction = block.instructions()[index];
    if (!isI32Store(instruction)) {
      continue;
    }
    const auto address = pointerPath(instruction.operands().back(), definitions);
    if (!address.has_value() || !address->indices.empty() ||
        !isScalarGlobal(module, address->root)) {
      continue;
    }
    const auto valueDefinition = definitions.find(instruction.operands()[1]);
    if (valueDefinition == definitions.end() ||
        valueDefinition->second.block != block.name() ||
        valueDefinition->second.instruction->opcode() != "add" ||
        valueDefinition->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto& addOperands = valueDefinition->second.instruction->operands();
    std::string previous;
    if (isIntegerLiteral(addOperands[0], 1)) {
      previous = addOperands[1];
    } else if (isIntegerLiteral(addOperands[1], 1)) {
      previous = addOperands[0];
    } else {
      continue;
    }
    const auto previousDefinition = definitions.find(previous);
    if (previousDefinition == definitions.end() ||
        previousDefinition->second.block != block.name() ||
        !isI32Load(*previousDefinition->second.instruction) ||
        previousDefinition->second.instruction->operands().back() !=
            address->root) {
      continue;
    }
    if (result.has_value()) {
      return std::nullopt;
    }
    result = CounterUpdate{address->root, instruction.operands()[1], index};
  }
  return result;
}

struct ValueEquivalenceContext {
  const Function& function;
  const DefinitionMap& definitions;
};

bool isLocalScalarSlot(const std::string& address,
                       const DefinitionMap& definitions) {
  if (address.empty() || address.front() != '%') {
    return false;
  }
  const auto found = definitions.find(address);
  return found != definitions.end() && found->second.instruction != nullptr &&
         found->second.instruction->opcode() == "alloca" &&
         found->second.instruction->operands().size() == 1 &&
         found->second.instruction->operands().front().find('[') ==
             std::string::npos;
}

bool isStoreToAddress(const Instruction& instruction, const std::string& address) {
  return isI32Store(instruction) && instruction.operands().back() == address;
}

std::optional<std::pair<std::string, std::size_t>> latestStoreBefore(
    const Function& function, const std::string& blockName, std::size_t before,
    const std::string& address) {
  for (const auto& block : function.blocks()) {
    if (block.name() != blockName) {
      continue;
    }
    std::optional<std::pair<std::string, std::size_t>> result;
    const std::size_t limit = std::min(before, block.instructions().size());
    for (std::size_t index = 0; index < limit; ++index) {
      const auto& instruction = block.instructions()[index];
      if (isStoreToAddress(instruction, address)) {
        result = std::pair{instruction.operands()[1], index};
      }
    }
    return result;
  }
  return std::nullopt;
}

bool hasCallBetween(const Function& function, const std::string& blockName,
                   std::size_t first, std::size_t last) {
  for (const auto& block : function.blocks()) {
    if (block.name() != blockName) {
      continue;
    }
    const std::size_t begin = std::min(first + 1, block.instructions().size());
    const std::size_t end = std::min(last, block.instructions().size());
    for (std::size_t index = begin; index < end; ++index) {
      if (block.instructions()[index].opcode() == "call") {
        return true;
      }
    }
    return false;
  }
  return true;
}

std::vector<std::pair<std::string, std::pair<std::string, std::size_t>>>
storesToAddress(const Function& function, const std::string& address) {
  std::vector<std::pair<std::string, std::pair<std::string, std::size_t>>> result;
  for (const auto& block : function.blocks()) {
    for (std::size_t index = 0; index < block.instructions().size(); ++index) {
      const auto& instruction = block.instructions()[index];
      if (isStoreToAddress(instruction, address)) {
        result.emplace_back(instruction.operands()[1],
                            std::pair{block.name(), index});
      }
    }
  }
  return result;
}

bool localSlotHasOnlyDirectUses(const Function& function,
                                const std::string& slot) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t operand = 0; operand < instruction.operands().size();
           ++operand) {
        if (instruction.operands()[operand] != slot) {
          continue;
        }
        const bool loadAddress =
            isI32Load(instruction) &&
            operand + 1 == instruction.operands().size();
        const bool storeAddress =
            isI32Store(instruction) &&
            operand + 1 == instruction.operands().size();
        if (!loadAddress && !storeAddress) {
          return false;
        }
      }
    }
  }
  return true;
}

bool blockDominates(const Function& function, const CFG& cfg,
                    const std::string& dominator,
                    const std::string& target) {
  if (dominator == target) {
    return true;
  }
  if (function.blocks().empty() || cfg.index.find(dominator) == cfg.index.end() ||
      cfg.index.find(target) == cfg.index.end()) {
    return false;
  }
  const std::string entry = function.blocks().front().name();
  std::vector<std::string> worklist{entry};
  std::unordered_set<std::string> visited;
  bool bypassReachable = false;
  while (!worklist.empty()) {
    const auto current = worklist.back();
    worklist.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    if (current == target) {
      bypassReachable = true;
      break;
    }
    if (current == dominator) {
      continue;
    }
    const auto found = cfg.index.find(current);
    if (found == cfg.index.end()) {
      continue;
    }
    for (const auto& successor : cfg.successors[found->second]) {
      worklist.push_back(successor);
    }
  }
  if (bypassReachable) {
    return false;
  }
  worklist = {entry};
  visited.clear();
  while (!worklist.empty()) {
    const auto current = worklist.back();
    worklist.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    if (current == target) {
      return true;
    }
    const auto found = cfg.index.find(current);
    if (found == cfg.index.end()) {
      continue;
    }
    for (const auto& successor : cfg.successors[found->second]) {
      worklist.push_back(successor);
    }
  }
  return false;
}

enum class PathSafety { kNoTarget, kSafe, kUnsafe };

PathSafety combinePathSafety(PathSafety lhs, PathSafety rhs) {
  if (lhs == PathSafety::kUnsafe || rhs == PathSafety::kUnsafe) {
    return PathSafety::kUnsafe;
  }
  if (lhs == PathSafety::kSafe || rhs == PathSafety::kSafe) {
    return PathSafety::kSafe;
  }
  return PathSafety::kNoTarget;
}

PathSafety pathsBetweenLoads(
    const Function& function, const CFG& cfg, const std::string& currentBlock,
    std::size_t startIndex, const std::string& targetBlock,
    std::size_t targetIndex, const std::string& address, bool clobbered,
    std::unordered_set<std::string>& visiting) {
  const auto blockFound = cfg.index.find(currentBlock);
  if (blockFound == cfg.index.end()) {
    return PathSafety::kNoTarget;
  }
  const auto& block = function.blocks()[blockFound->second];
  const std::string state = currentBlock + ":" + std::to_string(startIndex) +
                            ":" + (clobbered ? "1" : "0");
  if (!visiting.insert(state).second) {
    return PathSafety::kNoTarget;
  }
  const std::size_t limit =
      currentBlock == targetBlock
          ? std::min(targetIndex, block.instructions().size())
          : block.instructions().size();
  bool stateClobbered = clobbered;
  for (std::size_t index = std::min(startIndex, limit); index < limit; ++index) {
    const auto& instruction = block.instructions()[index];
    if (instruction.opcode() == "call" ||
        (isI32Store(instruction) &&
         instruction.operands().back() == address)) {
      stateClobbered = true;
    }
  }
  if (currentBlock == targetBlock) {
    visiting.erase(state);
    return stateClobbered ? PathSafety::kUnsafe : PathSafety::kSafe;
  }
  PathSafety result = PathSafety::kNoTarget;
  for (const auto& successor : cfg.successors[blockFound->second]) {
    result = combinePathSafety(
        result, pathsBetweenLoads(function, cfg, successor, 0, targetBlock,
                                  targetIndex, address, stateClobbered,
                                  visiting));
    if (result == PathSafety::kUnsafe) {
      break;
    }
  }
  visiting.erase(state);
  return result;
}

bool sameLocalSlotLoad(const ValueEquivalenceContext& context,
                       const Definition& left, const Definition& right,
                       const std::string& address) {
  if (!isLocalScalarSlot(address, context.definitions) ||
      !localSlotHasOnlyDirectUses(context.function, address)) {
    return false;
  }
  const auto cfg = buildCFG(context.function);
  const auto directEdgeSafe = [&](const Definition& from,
                                  const Definition& to) {
    const auto fromIndex = cfg.index.find(from.block);
    const auto toIndex = cfg.index.find(to.block);
    if (fromIndex == cfg.index.end() || toIndex == cfg.index.end() ||
        std::find(cfg.successors[fromIndex->second].begin(),
                  cfg.successors[fromIndex->second].end(), to.block) ==
            cfg.successors[fromIndex->second].end() ||
        cfg.predecessors[toIndex->second].size() != 1) {
      return false;
    }
    const auto* fromBlock = findBlock(context.function, cfg, from.block);
    const auto* toBlock = findBlock(context.function, cfg, to.block);
    if (fromBlock == nullptr || toBlock == nullptr) {
      return false;
    }
    for (std::size_t index = from.index + 1;
         index < fromBlock->instructions().size(); ++index) {
      const auto& instruction = fromBlock->instructions()[index];
      if (instruction.opcode() == "call" ||
          (isI32Store(instruction) &&
           instruction.operands().back() == address)) {
        return false;
      }
    }
    for (std::size_t index = 0;
         index < to.index && index < toBlock->instructions().size(); ++index) {
      const auto& instruction = toBlock->instructions()[index];
      if (instruction.opcode() == "call" ||
          (isI32Store(instruction) &&
           instruction.operands().back() == address)) {
        return false;
      }
    }
    return true;
  };
  if (left.block == right.block) {
    const std::size_t first = std::min(left.index, right.index);
    const std::size_t last = std::max(left.index, right.index);
    const auto* block = findBlock(context.function, cfg, left.block);
    if (block == nullptr) {
      return false;
    }
    for (std::size_t index = first + 1;
         index < last && index < block->instructions().size(); ++index) {
      const auto& instruction = block->instructions()[index];
      if (instruction.opcode() == "call" ||
          (isI32Store(instruction) &&
           instruction.operands().back() == address)) {
        return false;
      }
    }
    return true;
  }
  if (directEdgeSafe(left, right) || directEdgeSafe(right, left)) {
    return true;
  }
  if (blockDominates(context.function, cfg, left.block, right.block)) {
    std::unordered_set<std::string> visiting;
    return pathsBetweenLoads(context.function, cfg, left.block, left.index + 1,
                             right.block, right.index, address, false,
                             visiting) == PathSafety::kSafe;
  }
  if (blockDominates(context.function, cfg, right.block, left.block)) {
    std::unordered_set<std::string> visiting;
    return pathsBetweenLoads(context.function, cfg, right.block,
                             right.index + 1, left.block, left.index, address,
                             false, visiting) == PathSafety::kSafe;
  }
  return false;
}

std::optional<std::string> forwardedLoadValue(
    const std::string& value, const ValueEquivalenceContext& context,
    std::unordered_set<std::string>& visiting) {
  const auto found = context.definitions.find(value);
  if (found == context.definitions.end() || found->second.instruction == nullptr ||
      !isI32Load(*found->second.instruction) ||
      !visiting.insert(value).second) {
    return std::nullopt;
  }

  const auto& load = *found->second.instruction;
  const std::string address = load.operands().back();
  if (const auto local = latestStoreBefore(context.function, found->second.block,
                                           found->second.index, address);
      local.has_value() &&
      !hasCallBetween(context.function, found->second.block, local->second,
                      found->second.index)) {
    return local->first;
  }

  // A single entry-block store to a local scalar slot is a conservative
  // parameter/initializer forwarding rule.  We intentionally do not apply
  // this to globals or to slots with multiple stores.
  if (!isLocalScalarSlot(address, context.definitions)) {
    return std::nullopt;
  }
  const auto allStores = storesToAddress(context.function, address);
  if (allStores.size() != 1 || allStores.front().second.first !=
                                  context.function.blocks().front().name()) {
    return std::nullopt;
  }
  return allStores.front().first;
}

bool sameScalarValueImpl(const std::string& lhs, const std::string& rhs,
                         const ValueEquivalenceContext& context,
                         std::unordered_set<std::string>& visiting) {
  if (lhs == rhs) {
    return true;
  }

  long long leftLiteral = 0;
  long long rightLiteral = 0;
  if (detail::parseIntegerLiteral(lhs, leftLiteral) &&
      detail::parseIntegerLiteral(rhs, rightLiteral)) {
    return leftLiteral == rightLiteral;
  }

  const auto left = context.definitions.find(lhs);
  const auto right = context.definitions.find(rhs);
  if (left != context.definitions.end() && right != context.definitions.end() &&
      isI32Load(*left->second.instruction) &&
      isI32Load(*right->second.instruction) &&
      left->second.instruction->operands().back() ==
          right->second.instruction->operands().back()) {
    return sameLocalSlotLoad(context, left->second, right->second,
                             left->second.instruction->operands().back());
  }

  if (const auto forwarded = forwardedLoadValue(lhs, context, visiting);
      forwarded.has_value() && sameScalarValueImpl(*forwarded, rhs, context,
                                                   visiting)) {
    return true;
  }
  if (const auto forwarded = forwardedLoadValue(rhs, context, visiting);
      forwarded.has_value() && sameScalarValueImpl(lhs, *forwarded, context,
                                                   visiting)) {
    return true;
  }
  return false;
}

bool sameScalarValue(const std::string& lhs, const std::string& rhs,
                     const Function& function,
                     const DefinitionMap& definitions) {
  std::unordered_set<std::string> visiting;
  return sameScalarValueImpl(lhs, rhs,
                             ValueEquivalenceContext{function, definitions},
                             visiting);
}

bool validNodeUses(const Function& function, const ProducerCandidate& candidate,
                   const DefinitionMap& definitions,
                   const std::unordered_set<std::string>& allowedAddresses) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t operand = 0; operand < instruction.operands().size();
           ++operand) {
        if (instruction.operands()[operand] != candidate.nodeIndex) {
          continue;
        }
        bool allowed = false;
        if (instruction.opcode() == "store" && operand == 1 &&
            instruction.operands().size() == 3) {
          const auto address =
              pointerPath(instruction.operands().back(), definitions);
          if (address.has_value() &&
              ((address->root == candidate.counterRoot &&
                instruction.operands()[1] == candidate.nodeIndex) ||
               address->root == candidate.nextValueRoot ||
              address->root == candidate.valueRoot) &&
              ((address->root == candidate.nextValueRoot &&
                (address->indices.size() == 1 &&
                 sameScalarValue(address->indices.back(),
                                 candidate.currentNode, function,
                                 definitions))) ||
               (address->root == candidate.counterRoot &&
                address->indices.empty()))) {
            allowed = true;
          }
        }
        if (instruction.opcode() == "getelementptr" ||
            instruction.opcode() == "getelementptr.inbounds" ||
            instruction.opcode() == "getelementptr.linear" ||
            instruction.opcode() == "gep" || instruction.opcode() == "gep.linear") {
          if (instruction.hasResult() &&
              allowedAddresses.find(instruction.result()) !=
                  allowedAddresses.end()) {
            allowed = true;
          }
        }
        if (!allowed) {
          return false;
        }
      }
    }
  }
  return true;
}

std::vector<ProducerCandidate> findProducerCandidates(
    const Module& module, std::size_t functionIndex) {
  const auto& function = module.functions()[functionIndex];
  const bool debug = std::getenv("COMPILER_ASSOCIATIVE_CHAIN_AGGREGATION_DEBUG") !=
                     nullptr;
  if (function.returnType() != "int" || function.parameters().size() != 2 ||
      !hasParameter(function, function.parameters()[0].name) ||
      !hasParameter(function, function.parameters()[1].name)) {
    return {};
  }
  const auto definitions = buildDefinitions(function);
  const auto cfg = buildCFG(function);
  const auto parameters = parameterNames(function);
  std::vector<ProducerCandidate> candidates;

  for (const auto& compareBlock : function.blocks()) {
    const auto* terminator = compareBlock.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3 ||
        !blockInCycle(cfg, compareBlock.name())) {
      continue;
    }
    const auto condition = definitions.find(terminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second.block != compareBlock.name() ||
        condition->second.instruction->opcode() != "icmp.eq" ||
        condition->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto& compareOperands = condition->second.instruction->operands();
    std::string keyParameter;
    std::string keyLoad;
    if (parameters.count(compareOperands[0]) != 0 &&
        !compareOperands[1].empty() && compareOperands[1].front() == '%') {
      keyParameter = compareOperands[0];
      keyLoad = compareOperands[1];
    } else if (parameters.count(compareOperands[1]) != 0 &&
               !compareOperands[0].empty() && compareOperands[0].front() == '%') {
      keyParameter = compareOperands[1];
      keyLoad = compareOperands[0];
    } else {
      continue;
    }
    const auto keyDefinition = definitions.find(keyLoad);
    if (keyDefinition == definitions.end() ||
        !isI32Load(*keyDefinition->second.instruction)) {
      continue;
    }
    const auto keyPath = pointerPath(
        keyDefinition->second.instruction->operands().back(), definitions);
    if (!keyPath.has_value() || keyPath->indices.size() != 1 ||
        !isArrayGlobal(module, keyPath->root)) {
      continue;
    }
    const auto currentNode = keyPath->indices.back();
    if (debug) {
      std::cerr << "assoc-debug compare " << compareBlock.name()
                << " key=" << keyPath->root << " node=" << currentNode << '\n';
    }

    for (std::size_t successor = 1; successor < 3; ++successor) {
      const auto* duplicate =
          findBlock(function, cfg, terminator->operands()[successor]);
      if (duplicate == nullptr || duplicate->name() == compareBlock.name() ||
          duplicate->terminator() == nullptr ||
          duplicate->terminator()->opcode() != "ret" ||
          duplicate->terminator()->operands().size() != 1 ||
          !isIntegerLiteral(duplicate->terminator()->operands()[0], 1)) {
        continue;
      }
      const auto counter = findCounterUpdate(module, *duplicate, definitions);
      if (!counter.has_value()) {
        if (debug) {
          std::cerr << "assoc-debug no counter in " << duplicate->name() << '\n';
        }
        continue;
      }
      if (debug) {
        std::cerr << "assoc-debug counter root=" << counter->root
                  << " result=" << counter->result << '\n';
      }

      struct StoreRecord {
        std::size_t index = 0;
        PointerPath path;
        std::string value;
      };
      std::vector<StoreRecord> stores;
      bool hasCall = false;
      for (std::size_t index = 0; index < duplicate->instructions().size();
           ++index) {
        const auto& instruction = duplicate->instructions()[index];
        if (instruction.opcode() == "call") {
          hasCall = true;
          break;
        }
        if (!isI32Store(instruction)) {
          continue;
        }
        const auto path =
            pointerPath(instruction.operands().back(), definitions);
        if (!path.has_value()) {
          continue;
        }
        stores.push_back(StoreRecord{index, *path, instruction.operands()[1]});
      }
      if (hasCall) {
        continue;
      }

      std::string nextValueRoot;
      std::size_t nextValueNewStoreIndex = 0;
      std::size_t nextValueCurrentStoreIndex = 0;
      std::string oldNextValue;
      for (const auto& first : stores) {
        if (debug) {
          std::cerr << "assoc-debug store " << first.index << " root="
                    << first.path.root << " indices=" << first.path.indices.size();
          for (const auto& index : first.path.indices) {
            std::cerr << " [" << index << "]";
          }
          std::cerr << " value=" << first.value << '\n';
        }
        if (!isArrayGlobal(module, first.path.root) ||
            first.path.indices.size() != 1 ||
            !sameScalarValue(first.path.indices.back(), counter->result,
                             function, definitions)) {
          continue;
        }
        const auto oldDefinition = definitions.find(first.value);
        if (oldDefinition == definitions.end() ||
            !isI32Load(*oldDefinition->second.instruction)) {
          continue;
        }
        const auto oldPath = pointerPath(
            oldDefinition->second.instruction->operands().back(), definitions);
        if (!oldPath.has_value() || oldPath->indices.size() != 1 ||
            !sameScalarValue(oldPath->indices.back(), currentNode,
                             function, definitions) ||
            oldPath->root != first.path.root) {
          continue;
        }
        for (const auto& second : stores) {
          if (second.index == first.index ||
              second.path.root != first.path.root ||
              second.path.indices.size() != 1 ||
              !sameScalarValue(second.path.indices.back(), currentNode,
                               function, definitions) ||
              !sameScalarValue(second.value, counter->result, function,
                               definitions)) {
            continue;
          }
          if (!nextValueRoot.empty()) {
            nextValueRoot.clear();
            break;
          }
          nextValueRoot = first.path.root;
          nextValueNewStoreIndex = first.index;
          nextValueCurrentStoreIndex = second.index;
          oldNextValue = first.value;
        }
        if (nextValueRoot.empty() && !oldNextValue.empty()) {
          break;
        }
      }
      if (nextValueRoot.empty()) {
        if (debug) {
          std::cerr << "assoc-debug no next-value pair in " << duplicate->name()
                    << '\n';
        }
        continue;
      }

      std::string valueRoot;
      std::size_t valueStoreIndex = 0;
      std::string valueGepOpcode;
      const auto otherParameter =
          function.parameters()[0].name == keyParameter
              ? function.parameters()[1].name
              : function.parameters()[0].name;
      for (const auto& store : stores) {
        if (store.path.root == nextValueRoot ||
            !isArrayGlobal(module, store.path.root) ||
            store.path.indices.size() != 1 ||
            !sameScalarValue(store.path.indices.back(), counter->result,
                             function, definitions) ||
            !sameScalarValue(store.value, otherParameter, function,
                             definitions)) {
          continue;
        }
        if (!valueRoot.empty()) {
          valueRoot.clear();
          break;
        }
        valueRoot = store.path.root;
        valueStoreIndex = store.index;
        const auto addressDefinition = definitions.find(
            duplicate->instructions()[store.index].operands().back());
        if (addressDefinition == definitions.end()) {
          valueRoot.clear();
          break;
        }
        valueGepOpcode = addressDefinition->second.instruction->opcode();
      }
      if (valueRoot.empty() || valueRoot == keyPath->root ||
          valueRoot == nextValueRoot) {
        if (debug) {
          std::cerr << "assoc-debug no value store root=" << valueRoot << '\n';
        }
        continue;
      }
      const std::unordered_set<std::string> allowedAddresses{
          duplicate->instructions()[nextValueNewStoreIndex].operands().back(),
          duplicate->instructions()[nextValueCurrentStoreIndex]
              .operands()
              .back(),
          duplicate->instructions()[valueStoreIndex].operands().back()};
      ProducerCandidate candidate{functionIndex,
                                  duplicate->name(),
                                  keyParameter,
                                  otherParameter,
                                  counter->root,
                                  keyPath->root,
                                  valueRoot,
                                  nextValueRoot,
                                  counter->result,
                                  currentNode,
                                  valueGepOpcode,
                                  counter->storeIndex,
                                  nextValueNewStoreIndex,
                                  nextValueCurrentStoreIndex,
                                  valueStoreIndex};
      if (!validNodeUses(function, candidate, definitions, allowedAddresses)) {
        if (debug) {
          std::cerr << "assoc-debug invalid node uses\n";
        }
        continue;
      }
      candidates.push_back(std::move(candidate));
    }
  }
  return candidates;
}

bool isLocalSlot(const std::string& address, const std::string& slot) {
  return address == slot;
}

std::optional<ConsumerCandidate> findConsumerAt(
    const Module& module, std::size_t functionIndex,
    const ProducerCandidate& producer, const std::string& compareBlockName) {
  const auto& function = module.functions()[functionIndex];
  if (function.returnType() != "int" || function.parameters().size() != 1 ||
      function.parameters()[0].type != "int") {
    return std::nullopt;
  }
  const auto definitions = buildDefinitions(function);
  const auto cfg = buildCFG(function);
  const auto* compareBlock = findBlock(function, cfg, compareBlockName);
  if (compareBlock == nullptr || compareBlock->terminator() == nullptr ||
      compareBlock->terminator()->opcode() != "br" ||
      compareBlock->terminator()->operands().size() != 3) {
    return std::nullopt;
  }
  const auto condition =
      definitions.find(compareBlock->terminator()->operands()[0]);
  if (condition == definitions.end() ||
      condition->second.block != compareBlockName ||
      condition->second.instruction->opcode() != "icmp.eq" ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto& compareOperands = condition->second.instruction->operands();
  const auto parameter = function.parameters()[0].name;
  std::string keyLoad;
  if (compareOperands[0] == parameter) {
    keyLoad = compareOperands[1];
  } else if (compareOperands[1] == parameter) {
    keyLoad = compareOperands[0];
  } else {
    return std::nullopt;
  }
  const auto keyDefinition = definitions.find(keyLoad);
  if (keyDefinition == definitions.end() ||
      !isI32Load(*keyDefinition->second.instruction)) {
    return std::nullopt;
  }
  const auto keyPath = pointerPath(
      keyDefinition->second.instruction->operands().back(), definitions);
  if (!keyPath.has_value() || keyPath->root != producer.keyRoot ||
      keyPath->indices.size() != 1) {
    return std::nullopt;
  }
  const auto matchedNode = keyPath->indices.back();

  std::vector<ConsumerCandidate> candidates;
  for (std::size_t successor = 1; successor < 3; ++successor) {
    const auto* matchBlock =
        findBlock(function, cfg, compareBlock->terminator()->operands()[successor]);
    if (matchBlock == nullptr || matchBlock->terminator() == nullptr ||
        matchBlock->terminator()->opcode() != "br" ||
        matchBlock->terminator()->operands().size() != 1) {
      continue;
    }
    std::vector<std::string> slots;
    std::optional<std::pair<std::string, std::size_t>> accumulatorInit;
    std::optional<std::pair<std::string, std::size_t>> nodeInit;
    bool unsupported = false;
    for (std::size_t index = 0; index < matchBlock->instructions().size();
         ++index) {
      const auto& instruction = matchBlock->instructions()[index];
      if (instruction.opcode() == "call") {
        unsupported = true;
        break;
      }
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        slots.push_back(instruction.result());
        continue;
      }
      if (isI32Store(instruction) &&
          std::find(slots.begin(), slots.end(), instruction.operands().back()) !=
              slots.end()) {
        if (isIntegerLiteral(instruction.operands()[1], 0)) {
          if (accumulatorInit.has_value()) {
            unsupported = true;
            break;
          }
          accumulatorInit =
              std::pair{instruction.operands().back(), index};
        } else if (sameScalarValue(instruction.operands()[1], matchedNode,
                                   function, definitions)) {
          if (nodeInit.has_value()) {
            unsupported = true;
            break;
          }
          nodeInit = std::pair{instruction.operands().back(), index};
        } else {
          unsupported = true;
          break;
        }
        continue;
      }
      if (instruction.opcode() == "br") {
        continue;
      }
      unsupported = true;
      break;
    }
    if (unsupported || slots.size() != 2 || !accumulatorInit.has_value() ||
        !nodeInit.has_value() || accumulatorInit->first == nodeInit->first) {
      continue;
    }
    const auto* innerHeader =
        findBlock(function, cfg, matchBlock->terminator()->operands()[0]);
    if (innerHeader == nullptr || innerHeader->terminator() == nullptr ||
        innerHeader->terminator()->opcode() != "br" ||
        innerHeader->terminator()->operands().size() != 3) {
      continue;
    }
    const auto innerCondition =
        definitions.find(innerHeader->terminator()->operands()[0]);
    if (innerCondition == definitions.end() ||
        innerCondition->second.block != innerHeader->name() ||
        innerCondition->second.instruction->opcode() != "icmp.ne" ||
        innerCondition->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto& innerOperands = innerCondition->second.instruction->operands();
    std::string nodeLoad;
    if (isIntegerLiteral(innerOperands[0], 0)) {
      nodeLoad = innerOperands[1];
    } else if (isIntegerLiteral(innerOperands[1], 0)) {
      nodeLoad = innerOperands[0];
    } else {
      continue;
    }
    const auto nodeLoadDefinition = definitions.find(nodeLoad);
    if (nodeLoadDefinition == definitions.end() ||
        !isI32Load(*nodeLoadDefinition->second.instruction) ||
        !isLocalSlot(
            nodeLoadDefinition->second.instruction->operands().back(),
            nodeInit->first)) {
      continue;
    }

    const std::string innerBodyName = innerHeader->terminator()->operands()[1];
    const std::string innerExitName = innerHeader->terminator()->operands()[2];
    const auto* innerBody = findBlock(function, cfg, innerBodyName);
    const auto* innerExit = findBlock(function, cfg, innerExitName);
    if (innerBody == nullptr || innerExit == nullptr ||
        innerBody->terminator() == nullptr ||
        innerBody->terminator()->opcode() != "br" ||
        innerBody->terminator()->operands().size() != 1 ||
        innerBody->terminator()->operands()[0] != innerHeader->name() ||
        std::find(cfg.predecessors[cfg.index.at(innerHeader->name())].begin(),
                  cfg.predecessors[cfg.index.at(innerHeader->name())].end(),
                  matchBlock->name()) ==
            cfg.predecessors[cfg.index.at(innerHeader->name())].end() ||
        innerExit->instructions().empty() ||
        std::any_of(innerExit->instructions().begin(),
                    innerExit->instructions().end(),
                    [](const Instruction& instruction) {
                      return instruction.opcode() == "phi";
                    })) {
      continue;
    }

    std::string accumulatorLoad;
    std::string bodyNodeLoad;
    std::string valueLoad;
    std::string nextValueLoad;
    std::string valueIndex;
    std::string nextIndex;
    std::string addResult;
    std::string accumulatorStoreValue;
    std::string nodeStoreValue;
    std::string valueGepOpcode;
    bool bodyUnsupported = false;
    for (const auto& instruction : innerBody->instructions()) {
      if (instruction.opcode() == "call") {
        bodyUnsupported = true;
        break;
      }
      if (isI32Load(instruction)) {
        const auto& address = instruction.operands().back();
        if (address == accumulatorInit->first) {
          if (!accumulatorLoad.empty()) {
            bodyUnsupported = true;
            break;
          }
          accumulatorLoad = instruction.result();
          continue;
        }
        if (address == nodeInit->first) {
          if (bodyNodeLoad.empty()) {
            bodyNodeLoad = instruction.result();
          }
          continue;
        }
        const auto path = pointerPath(address, definitions);
        if (!path.has_value() || path->indices.size() != 1 ||
            (path->root != producer.valueRoot &&
             path->root != producer.nextValueRoot)) {
          bodyUnsupported = true;
          break;
        }
        if (path->root == producer.valueRoot) {
          if (!valueLoad.empty()) {
            bodyUnsupported = true;
            break;
          }
          valueLoad = instruction.result();
          valueIndex = path->indices.back();
          const auto addressDefinition = definitions.find(address);
          if (addressDefinition == definitions.end()) {
            bodyUnsupported = true;
            break;
          }
          valueGepOpcode = addressDefinition->second.instruction->opcode();
        } else {
          if (!nextValueLoad.empty()) {
            bodyUnsupported = true;
            break;
          }
          nextValueLoad = instruction.result();
          nextIndex = path->indices.back();
        }
        continue;
      }
      if (instruction.opcode() == "add" && instruction.hasResult() &&
          instruction.operands().size() == 2) {
        if (!addResult.empty()) {
          bodyUnsupported = true;
          break;
        }
        addResult = instruction.result();
        continue;
      }
      if (isI32Store(instruction)) {
        if (instruction.operands().back() == accumulatorInit->first) {
          if (!accumulatorStoreValue.empty()) {
            bodyUnsupported = true;
            break;
          }
          accumulatorStoreValue = instruction.operands()[1];
          continue;
        }
        if (instruction.operands().back() == nodeInit->first) {
          if (!nodeStoreValue.empty()) {
            bodyUnsupported = true;
            break;
          }
          nodeStoreValue = instruction.operands()[1];
          continue;
        }
        bodyUnsupported = true;
        break;
      }
      if (isGep(instruction.opcode()) || instruction.opcode() == "br") {
        continue;
      }
      bodyUnsupported = true;
      break;
    }
    if (bodyUnsupported || accumulatorLoad.empty() || bodyNodeLoad.empty() ||
        valueLoad.empty() || nextValueLoad.empty() || addResult.empty() ||
        !sameScalarValue(accumulatorStoreValue, addResult, function, definitions) ||
        !sameScalarValue(nodeStoreValue, nextValueLoad, function, definitions) ||
        valueIndex.empty() ||
        !sameScalarValue(valueIndex, nextIndex, function, definitions) ||
        !((std::find(definitions.at(addResult).instruction->operands().begin(),
                     definitions.at(addResult).instruction->operands().end(),
                     accumulatorLoad) !=
           definitions.at(addResult).instruction->operands().end()) &&
          (std::find(definitions.at(addResult).instruction->operands().begin(),
                     definitions.at(addResult).instruction->operands().end(),
                     valueLoad) !=
                     definitions.at(addResult).instruction->operands().end()))) {
      continue;
    }

    const auto predecessorList = cfg.predecessors[cfg.index.at(innerHeader->name())];
    if (predecessorList.size() != 2 ||
        std::count(predecessorList.begin(), predecessorList.end(),
                  matchBlock->name()) != 1 ||
        std::count(predecessorList.begin(), predecessorList.end(),
                  innerBody->name()) != 1) {
      continue;
    }
    const auto removed = std::unordered_set<std::string>{innerHeader->name(),
                                                         innerBody->name()};
    bool slotEscapes = false;
    for (const auto& block : function.blocks()) {
      if (removed.count(block.name()) != 0 ||
          block.name() == matchBlock->name()) {
        continue;
      }
      for (const auto& instruction : block.instructions()) {
        for (const auto& operand : instruction.operands()) {
          if (operand == nodeInit->first) {
            slotEscapes = true;
          }
        }
      }
    }
    if (slotEscapes) {
      continue;
    }

    ConsumerCandidate candidate{functionIndex,
                                matchBlock->name(),
                                innerHeader->name(),
                                innerBody->name(),
                                innerExit->name(),
                                accumulatorInit->first,
                                nodeInit->first,
                                matchedNode,
                                valueGepOpcode,
                                accumulatorInit->second,
                                nodeInit->second};
    candidates.push_back(std::move(candidate));
  }
  if (candidates.size() != 1) {
    return std::nullopt;
  }
  return candidates.front();
}

std::vector<ConsumerCandidate> findConsumers(
    const Module& module, std::size_t functionIndex,
    const ProducerCandidate& producer) {
  const auto& function = module.functions()[functionIndex];
  const auto cfg = buildCFG(function);
  std::vector<ConsumerCandidate> candidates;
  for (const auto& block : function.blocks()) {
    if (block.terminator() == nullptr || blockInCycle(cfg, block.name())) {
      const auto candidate = findConsumerAt(module, functionIndex, producer,
                                            block.name());
      if (candidate.has_value()) {
        candidates.push_back(*candidate);
      }
    }
  }
  return candidates;
}

std::unordered_map<std::string, std::string> derivedPointers(
    const Function& function) {
  std::unordered_map<std::string, std::string> roots;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && !instruction.result().empty() &&
          instruction.result().front() == '@') {
        roots.emplace(instruction.result(), instruction.result());
      }
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult() || instruction.operands().empty()) {
          continue;
        }
        if (isProjection(instruction.opcode())) {
          const auto found = roots.find(instruction.operands().front());
          if (found != roots.end()) {
            changed |= roots.emplace(instruction.result(), found->second).second;
          }
        } else if (instruction.opcode() == "select" ||
                   instruction.opcode() == "phi") {
          for (const auto& operand : instruction.operands()) {
            const auto incoming = roots.find(operand);
            if (incoming != roots.end()) {
              changed |= roots.emplace(instruction.result(), incoming->second).second;
              break;
            }
          }
        }
      }
    }
  }
  return roots;
}

bool relevantUsesAreSafe(const Module& module,
                         const std::unordered_set<std::size_t>& allowedFunctions,
                         const std::unordered_set<std::string>& relevantRoots) {
  std::vector<std::unordered_map<std::string, std::string>> pointerRoots;
  pointerRoots.reserve(module.functions().size());
  for (const auto& function : module.functions()) {
    auto roots = derivedPointers(function);
    for (const auto& global : module.globals()) {
      roots.emplace("@" + global.name(), "@" + global.name());
    }
    pointerRoots.push_back(std::move(roots));
  }

  for (std::size_t functionIndex = 0; functionIndex < module.functions().size();
       ++functionIndex) {
    const auto& function = module.functions()[functionIndex];
    const auto& roots = pointerRoots[functionIndex];
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        for (std::size_t operand = 0; operand < instruction.operands().size();
             ++operand) {
          const auto found = roots.find(instruction.operands()[operand]);
          if (found == roots.end() ||
              relevantRoots.count(found->second) == 0) {
            continue;
          }
          if (allowedFunctions.count(functionIndex) == 0) {
            return false;
          }
          const bool projectionUse =
              (isProjection(instruction.opcode()) && operand == 0) ||
              (instruction.opcode() == "select" ||
               instruction.opcode() == "phi");
          const bool loadUse = instruction.opcode() == "load" &&
                               operand + 1 == instruction.operands().size() &&
                               isI32Load(instruction);
          const bool storeUse = instruction.opcode() == "store" &&
                                operand + 1 == instruction.operands().size() &&
                                isI32Store(instruction);
          if (!projectionUse && !loadUse && !storeUse) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

bool producerArrayStoresAreSafe(
    const Module& module, const Function& function,
    const ProducerCandidate& candidate, const DefinitionMap& definitions) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (!isI32Store(instruction)) {
        continue;
      }
      const auto path = pointerPath(instruction.operands().back(), definitions);
      if (!path.has_value() || path->indices.size() != 1) {
        continue;
      }
      if (path->root != candidate.nextValueRoot &&
          path->root != candidate.valueRoot) {
        continue;
      }
      const bool duplicate = block.name() == candidate.duplicateBlock;
      const bool recognizedDuplicate =
          duplicate &&
          ((&instruction == &block.instructions()[candidate.nextValueNewStoreIndex]) ||
           (&instruction ==
            &block.instructions()[candidate.nextValueCurrentStoreIndex]) ||
           (&instruction == &block.instructions()[candidate.valueStoreIndex]));
      if (recognizedDuplicate) {
        continue;
      }
      const auto update =
          findCounterUpdate(module, block, definitions);
      if (!update.has_value() || update->root != candidate.counterRoot ||
          !sameScalarValue(path->indices.back(), update->result, function,
                           definitions) ||
          (!isIntegerLiteral(instruction.operands()[1], 0) &&
           path->root == candidate.nextValueRoot)) {
        if (path->root == candidate.nextValueRoot ||
            path->root == candidate.valueRoot) {
          return false;
        }
      }
      if (path->root == candidate.nextValueRoot &&
          !isIntegerLiteral(instruction.operands()[1], 0)) {
        return false;
      }
      if (path->root == candidate.valueRoot &&
          (update == std::nullopt ||
           !sameScalarValue(path->indices.back(), update->result, function,
                            definitions))) {
        return false;
      }
    }
  }
  return true;
}

std::unordered_set<std::string> collectRelevantRoots(
    const Module& module, const Function& producer, const Function& consumer,
    const ProducerCandidate& candidate) {
  std::unordered_set<std::string> roots{candidate.keyRoot,
                                        candidate.valueRoot,
                                        candidate.nextValueRoot};
  for (const auto* function : {&producer, &consumer}) {
    const auto definitions = buildDefinitions(*function);
    for (const auto& block : function->blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() != "load" &&
            instruction.opcode() != "store") {
          continue;
        }
        if (instruction.operands().empty()) {
          continue;
        }
        const auto path = pointerPath(instruction.operands().back(), definitions);
        if (path.has_value() && isArrayGlobal(module, path->root)) {
          roots.insert(path->root);
        }
      }
    }
  }
  return roots;
}

std::optional<PairCandidate> findPair(const Module& module) {
  const bool debug = std::getenv("COMPILER_ASSOCIATIVE_CHAIN_AGGREGATION_DEBUG") !=
                     nullptr;
  for (std::size_t producerIndex = 0;
       producerIndex < module.functions().size(); ++producerIndex) {
    const auto producerCandidates =
        findProducerCandidates(module, producerIndex);
    if (debug) {
      std::cerr << "assoc-debug producer-index " << producerIndex
                << " candidates=" << producerCandidates.size() << '\n';
    }
    if (producerCandidates.size() != 1) {
      continue;
    }
    const auto& producer = producerCandidates.front();
    std::vector<ConsumerCandidate> consumers;
    for (std::size_t consumerIndex = 0;
         consumerIndex < module.functions().size(); ++consumerIndex) {
      if (consumerIndex == producerIndex) {
        continue;
      }
      const auto found = findConsumers(module, consumerIndex, producer);
      consumers.insert(consumers.end(), found.begin(), found.end());
      if (debug && !found.empty()) {
        std::cerr << "assoc-debug consumer-index " << consumerIndex
                  << " candidates=" << found.size() << '\n';
      }
    }
    if (consumers.size() != 1) {
      continue;
    }
    const auto& producerFunction = module.functions()[producerIndex];
    const auto& consumerFunction = module.functions()[consumers.front().functionIndex];
    const auto producerDefinitions = buildDefinitions(producerFunction);
    if (!producerArrayStoresAreSafe(module, producerFunction, producer,
                                    producerDefinitions)) {
      continue;
    }
    const auto relevantRoots =
        collectRelevantRoots(module, producerFunction, consumerFunction, producer);
    const std::unordered_set<std::size_t> allowedFunctions{
        producerIndex, consumers.front().functionIndex};
    if (!relevantUsesAreSafe(module, allowedFunctions, relevantRoots)) {
      continue;
    }
    return PairCandidate{producer, consumers.front(), relevantRoots};
  }
  return std::nullopt;
}

std::string freshValue(const Function& function, const std::string& stem,
                       std::unordered_set<std::string>& names) {
  if (names.empty()) {
    for (const auto& parameter : function.parameters()) {
      names.insert(parameter.name);
    }
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.hasResult()) {
          names.insert(instruction.result());
        }
      }
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const auto value = "%" + stem + "." + std::to_string(suffix);
    if (names.insert(value).second) {
      return value;
    }
  }
}

bool applyProducer(Module& module, const ProducerCandidate& candidate) {
  auto& function = module.functions()[candidate.functionIndex];
  auto* block = findBlock(function, candidate.duplicateBlock);
  if (block == nullptr ||
      candidate.nextValueNewStoreIndex >= block->instructions().size() ||
      candidate.nextValueCurrentStoreIndex >= block->instructions().size() ||
      candidate.valueStoreIndex >= block->instructions().size()) {
    return false;
  }
  std::unordered_set<std::string> names;
  const auto address = freshValue(function, "assoc.chain.aggregate.address", names);
  const auto oldValue = freshValue(function, "assoc.chain.aggregate.old", names);
  const auto sum = freshValue(function, "assoc.chain.aggregate.sum", names);
  const std::unordered_set<std::size_t> removed{
      candidate.nextValueNewStoreIndex,
      candidate.nextValueCurrentStoreIndex,
      candidate.valueStoreIndex};
  const auto first =
      *std::min_element(removed.begin(), removed.end());
  std::vector<Instruction> rewritten;
  rewritten.reserve(block->instructions().size() + 4);
  for (std::size_t index = 0; index < block->instructions().size(); ++index) {
    if (index == first) {
      rewritten.emplace_back(candidate.valueGepOpcode,
                              std::vector<std::string>{candidate.valueRoot,
                                                       candidate.currentNode},
                              address);
      rewritten.emplace_back("load", std::vector<std::string>{"int", address},
                             oldValue);
      rewritten.emplace_back("add",
                             std::vector<std::string>{oldValue,
                                                      candidate.valueParameter},
                             sum);
      rewritten.emplace_back(
          "store", std::vector<std::string>{"int", sum, address});
    }
    if (removed.count(index) != 0) {
      continue;
    }
    rewritten.push_back(block->instructions()[index]);
  }
  block->instructions() = std::move(rewritten);
  return true;
}

bool applyConsumer(Module& module, const ConsumerCandidate& candidate,
                   const ProducerCandidate& producer) {
  auto& function = module.functions()[candidate.functionIndex];
  auto* match = findBlock(function, candidate.matchBlock);
  if (match == nullptr || match->instructions().empty()) {
    return false;
  }
  std::unordered_set<std::string> names;
  const auto address = freshValue(function, "assoc.chain.direct.address", names);
  const auto aggregate = freshValue(function, "assoc.chain.direct.value", names);
  std::vector<Instruction> rewritten;
  rewritten.reserve(match->instructions().size() + 3);
  for (std::size_t index = 0; index < match->instructions().size(); ++index) {
    const auto& instruction = match->instructions()[index];
    if (instruction.opcode() == "alloca" &&
        instruction.result() == candidate.nodeSlot) {
      continue;
    }
    if (isI32Store(instruction) &&
        instruction.operands().back() == candidate.nodeSlot) {
      continue;
    }
    if (isI32Store(instruction) &&
        instruction.operands().back() == candidate.accumulatorSlot &&
        isIntegerLiteral(instruction.operands()[1], 0)) {
      continue;
    }
    if (index + 1 == match->instructions().size()) {
      rewritten.emplace_back(candidate.valueGepOpcode,
                             std::vector<std::string>{producer.valueRoot,
                                                      candidate.matchedNode},
                             address);
      rewritten.emplace_back("load", std::vector<std::string>{"int", address},
                             aggregate);
      rewritten.emplace_back(
          "store", std::vector<std::string>{"int", aggregate,
                                             candidate.accumulatorSlot});
      rewritten.emplace_back("br",
                             std::vector<std::string>{candidate.innerExit});
      continue;
    }
    rewritten.push_back(instruction);
  }
  match->instructions() = std::move(rewritten);

  auto& blocks = function.blocks();
  const std::unordered_set<std::string> removed{candidate.innerHeader,
                                                candidate.innerBody};
  blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                              [&](const BasicBlock& block) {
                                return removed.count(block.name()) != 0;
                              }),
               blocks.end());
  return true;
}

}  // namespace

void AssociativeChainAggregationPass::run(Module& module) {
  if (std::getenv("COMPILER_DISABLE_ASSOCIATIVE_CHAIN_AGGREGATION") !=
      nullptr) {
    return;
  }
  std::size_t transformed = 0;
  while (const auto candidate = findPair(module)) {
    if (!applyProducer(module, candidate->producer) ||
        !applyConsumer(module, candidate->consumer, candidate->producer)) {
      break;
    }
    ++transformed;
    module.addPassNote(
        "aggregated a proven associative integer chain into its root value");
  }
  if (std::getenv("COMPILER_ASSOCIATIVE_CHAIN_AGGREGATION_STATS") != nullptr) {
    std::cerr << "associative-chain-aggregation: transformed=" << transformed
              << '\n';
  }
}

}  // namespace compiler::midend::pass
