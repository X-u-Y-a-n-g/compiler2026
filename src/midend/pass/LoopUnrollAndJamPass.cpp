#include "../../../include/compiler/midend/pass/LoopUnrollAndJamPass.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <functional>
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

bool isGepOpcode(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "getelementptr.linear" ||
         opcode == "gep" || opcode == "gep.linear";
}

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> predecessors;
};

struct LoopCondition {
  std::string slot;
  std::string value;
  std::string bound;
};

struct PointerPath {
  std::string base;
  std::vector<std::string> indices;
};

struct Candidate {
  std::string outerHeader;
  std::string outerBody;
  std::string outerExit;
  std::string innerHeader;
  std::string innerBody;
  std::string innerExit;
  std::string outerSlot;
  std::string innerSlot;
  std::string reductionSlot;
  std::string outerValue;
  std::string outerBound;
  std::string reductionType;
  std::string reductionIdentity;
  std::string destination;
};

using DefinitionMap = std::unordered_map<std::string, const Instruction*>;

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (const auto& block : function.blocks()) {
    const auto* terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto& operands = terminator->operands();
    const auto addTarget = [&](const std::string& target) {
      const auto found = cfg.index.find(target);
      if (found != cfg.index.end()) {
        cfg.predecessors[found->second].push_back(block.name());
      }
    };
    if (operands.size() == 1) {
      addTarget(operands[0]);
    } else if (operands.size() == 3) {
      addTarget(operands[1]);
      if (operands[2] != operands[1]) {
        addTarget(operands[2]);
      }
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
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

bool isLoadFrom(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "load" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         instruction.operands()[1] == slot;
}

bool isStoreTo(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "store" &&
         instruction.operands().size() == 3 &&
         instruction.operands()[2] == slot;
}

std::optional<LoopCondition> loopCondition(const BasicBlock& block,
                                           const DefinitionMap& definitions) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3) {
    return std::nullopt;
  }
  const auto conditionDefinition = definitions.find(terminator->operands()[0]);
  if (conditionDefinition == definitions.end() ||
      conditionDefinition->second->opcode() != "icmp.slt" ||
      conditionDefinition->second->operands().size() != 2) {
    return std::nullopt;
  }
  const auto loadDefinition =
      definitions.find(conditionDefinition->second->operands()[0]);
  if (loadDefinition == definitions.end() ||
      loadDefinition->second->opcode() != "load" ||
      loadDefinition->second->operands().size() != 2) {
    return std::nullopt;
  }
  return LoopCondition{loadDefinition->second->operands()[1],
                       loadDefinition->second->result(),
                       conditionDefinition->second->operands()[1]};
}

bool containsName(const std::vector<std::string>& names,
                  const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

bool hasUnitUpdate(const BasicBlock& block, const std::string& slot,
                   const DefinitionMap& definitions) {
  for (const auto& instruction : block.instructions()) {
    if (!isStoreTo(instruction, slot)) {
      continue;
    }
    const auto addDefinition = definitions.find(instruction.operands()[1]);
    if (addDefinition == definitions.end() ||
        addDefinition->second->opcode() != "add" ||
        addDefinition->second->operands().size() != 2) {
      continue;
    }
    const auto& operands = addDefinition->second->operands();
    const auto isUnitOperand = [](const std::string& operand) {
      long long value = 0;
      return detail::parseIntegerLiteral(operand, value) && value == 1;
    };
    const auto otherLoadsSlot = [&](std::size_t valueIndex,
                                    std::size_t unitIndex) {
      if (!isUnitOperand(operands[unitIndex])) {
        return false;
      }
      const auto load = definitions.find(operands[valueIndex]);
      return load != definitions.end() && isLoadFrom(*load->second, slot);
    };
    if (otherLoadsSlot(0, 1) || otherLoadsSlot(1, 0)) {
      return true;
    }
  }
  return false;
}

bool hasConstantStore(const BasicBlock& block, const std::string& slot,
                      const std::string& expected) {
  long long expectedValue = 0;
  if (!detail::parseIntegerLiteral(expected, expectedValue)) {
    return false;
  }
  for (const auto& instruction : block.instructions()) {
    if (!isStoreTo(instruction, slot)) {
      continue;
    }
    long long value = 0;
    if (detail::parseIntegerLiteral(instruction.operands()[1], value) &&
        value == expectedValue) {
      return true;
    }
  }
  return false;
}

bool valueDependsOn(const std::string& value, const std::string& target,
                    const DefinitionMap& definitions,
                    std::unordered_set<std::string>& visiting) {
  if (value == target) {
    return true;
  }
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return false;
  }
  const auto definition = definitions.find(value);
  if (definition == definitions.end()) {
    return false;
  }
  for (const auto& operand : definition->second->operands()) {
    if (valueDependsOn(operand, target, definitions, visiting)) {
      return true;
    }
  }
  return false;
}

bool valueDependsOn(const std::string& value, const std::string& target,
                    const DefinitionMap& definitions) {
  std::unordered_set<std::string> visiting;
  return valueDependsOn(value, target, definitions, visiting);
}

std::unordered_set<std::string> collectScalarSlots(const Function& function) {
  std::unordered_set<std::string> slots;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult() &&
          instruction.operands().size() == 1 &&
          instruction.operands()[0].find('[') == std::string::npos) {
        slots.insert(instruction.result());
      }
    }
  }
  return slots;
}

std::optional<std::string> allocaType(const std::string& slot,
                                      const DefinitionMap& definitions) {
  const auto definition = definitions.find(slot);
  if (definition == definitions.end() ||
      definition->second->opcode() != "alloca" ||
      definition->second->operands().size() != 1 ||
      definition->second->operands()[0].find('[') != std::string::npos) {
    return std::nullopt;
  }
  return definition->second->operands()[0];
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
  const auto& instruction = *definition->second;
  const auto& opcode = instruction.opcode();
  if (!isGepOpcode(opcode) || instruction.operands().size() != 2) {
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

bool isValueLoadedFromSlot(const std::string& value, const std::string& slot,
                           const DefinitionMap& definitions) {
  const auto definition = definitions.find(value);
  return definition != definitions.end() &&
         isLoadFrom(*definition->second, slot);
}

bool hasUnsupportedControlOrCall(const BasicBlock& block) {
  for (const auto& instruction : block.instructions()) {
    if (instruction.opcode() == "call" || instruction.opcode() == "ret" ||
        instruction.opcode() == "phi") {
      return true;
    }
  }
  return false;
}

std::optional<std::string> reductionLoadResult(const BasicBlock& block,
                                               const std::string& slot) {
  std::optional<std::string> result;
  for (const auto& instruction : block.instructions()) {
    if (!isLoadFrom(instruction, slot)) {
      continue;
    }
    if (result.has_value()) {
      return std::nullopt;
    }
    result = instruction.result();
  }
  return result;
}

bool reductionIsUpdated(const BasicBlock& innerBody, const std::string& slot,
                        const DefinitionMap& definitions) {
  const auto loaded = reductionLoadResult(innerBody, slot);
  if (!loaded.has_value()) {
    return false;
  }
  for (const auto& instruction : innerBody.instructions()) {
    if (isStoreTo(instruction, slot) &&
        valueDependsOn(instruction.operands()[1], *loaded, definitions)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> destinationAddress(
    const BasicBlock& innerExit, const std::string& outerSlot,
    const std::string& reductionSlot,
    const std::unordered_set<std::string>& scalarSlots,
    const DefinitionMap& definitions) {
  const auto loaded = reductionLoadResult(innerExit, reductionSlot);
  if (!loaded.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> destination;
  for (const auto& instruction : innerExit.instructions()) {
    if (instruction.opcode() != "store" || instruction.operands().size() != 3) {
      continue;
    }
    const auto& address = instruction.operands()[2];
    if (address == outerSlot ||
        scalarSlots.find(address) != scalarSlots.end()) {
      continue;
    }
    if (!valueDependsOn(instruction.operands()[1], *loaded, definitions) ||
        destination.has_value()) {
      return std::nullopt;
    }
    destination = address;
  }
  return destination;
}

bool memoryDependencesPermitJamming(
    const BasicBlock& innerBody, const std::string& destination,
    const std::string& outerSlot,
    const std::unordered_set<std::string>& scalarSlots,
    const DefinitionMap& definitions) {
  const auto destinationPath = pointerPath(destination, definitions);
  if (!destinationPath.has_value() || destinationPath->indices.empty() ||
      !isValueLoadedFromSlot(destinationPath->indices.back(), outerSlot,
                             definitions)) {
    return false;
  }

  bool foundSameObjectColumnLoad = false;
  for (const auto& instruction : innerBody.instructions()) {
    if (instruction.opcode() == "store" && instruction.operands().size() == 3 &&
        scalarSlots.find(instruction.operands()[2]) == scalarSlots.end()) {
      return false;
    }
    if (instruction.opcode() != "load" || instruction.operands().size() != 2) {
      continue;
    }
    const auto& address = instruction.operands()[1];
    if (scalarSlots.find(address) != scalarSlots.end()) {
      continue;
    }
    const auto loadPath = pointerPath(address, definitions);
    if (!loadPath.has_value()) {
      return false;
    }
    if (loadPath->base != destinationPath->base) {
      continue;
    }
    if (loadPath->indices.size() != destinationPath->indices.size() ||
        loadPath->indices.empty() ||
        !isValueLoadedFromSlot(loadPath->indices.back(), outerSlot,
                               definitions)) {
      return false;
    }
    foundSameObjectColumnLoad = true;
  }
  return foundSameObjectColumnLoad;
}

std::optional<Candidate> findCandidate(const Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto scalarSlots = collectScalarSlots(function);

  for (const auto& outerHeader : function.blocks()) {
    const auto outerCondition = loopCondition(outerHeader, definitions);
    const auto* outerTerminator = outerHeader.terminator();
    if (!outerCondition.has_value() || outerTerminator == nullptr ||
        hasUnsupportedControlOrCall(outerHeader)) {
      continue;
    }
    const std::string outerBodyName = outerTerminator->operands()[1];
    const std::string outerExitName = outerTerminator->operands()[2];
    const auto* outerBody = findBlock(function, cfg, outerBodyName);
    const auto* outerExit = findBlock(function, cfg, outerExitName);
    if (outerBody == nullptr || outerExit == nullptr ||
        outerBody->terminator() == nullptr ||
        outerBody->terminator()->opcode() != "br" ||
        outerBody->terminator()->operands().size() != 1 ||
        hasUnsupportedControlOrCall(*outerBody) ||
        (!outerExit->instructions().empty() &&
         outerExit->instructions().front().opcode() == "phi")) {
      continue;
    }
    const std::string innerHeaderName = outerBody->terminator()->operands()[0];
    const auto* innerHeader = findBlock(function, cfg, innerHeaderName);
    if (innerHeader == nullptr || hasUnsupportedControlOrCall(*innerHeader)) {
      continue;
    }
    const auto innerCondition = loopCondition(*innerHeader, definitions);
    const auto* innerTerminator = innerHeader->terminator();
    if (!innerCondition.has_value() || innerTerminator == nullptr) {
      continue;
    }
    const std::string innerBodyName = innerTerminator->operands()[1];
    const std::string innerExitName = innerTerminator->operands()[2];
    const auto* innerBody = findBlock(function, cfg, innerBodyName);
    const auto* innerExit = findBlock(function, cfg, innerExitName);
    if (innerBody == nullptr || innerExit == nullptr ||
        innerBody->terminator() == nullptr ||
        innerExit->terminator() == nullptr ||
        innerBody->terminator()->opcode() != "br" ||
        innerBody->terminator()->operands().size() != 1 ||
        innerBody->terminator()->operands()[0] != innerHeaderName ||
        innerExit->terminator()->opcode() != "br" ||
        innerExit->terminator()->operands().size() != 1 ||
        innerExit->terminator()->operands()[0] != outerHeader.name() ||
        hasUnsupportedControlOrCall(*innerBody) ||
        hasUnsupportedControlOrCall(*innerExit)) {
      continue;
    }

    const auto outerHeaderIndex = cfg.index.at(outerHeader.name());
    const auto outerBodyIndex = cfg.index.at(outerBodyName);
    const auto innerHeaderIndex = cfg.index.at(innerHeaderName);
    const auto innerBodyIndex = cfg.index.at(innerBodyName);
    const auto innerExitIndex = cfg.index.at(innerExitName);
    if (cfg.predecessors[outerHeaderIndex].size() != 2 ||
        cfg.predecessors[outerBodyIndex] !=
            std::vector<std::string>{outerHeader.name()} ||
        cfg.predecessors[innerBodyIndex] !=
            std::vector<std::string>{innerHeaderName} ||
        cfg.predecessors[innerExitIndex] !=
            std::vector<std::string>{innerHeaderName} ||
        cfg.predecessors[innerHeaderIndex].size() != 2 ||
        !containsName(cfg.predecessors[innerHeaderIndex], outerBodyName) ||
        !containsName(cfg.predecessors[innerHeaderIndex], innerBodyName)) {
      continue;
    }
    std::string outerPreheader;
    for (const auto& predecessor : cfg.predecessors[outerHeaderIndex]) {
      if (predecessor != innerExitName) {
        outerPreheader = predecessor;
      }
    }
    const auto* preheader = findBlock(function, cfg, outerPreheader);
    if (preheader == nullptr ||
        !hasConstantStore(*preheader, outerCondition->slot, "0") ||
        !hasConstantStore(*outerBody, innerCondition->slot, "0") ||
        !hasUnitUpdate(*innerBody, innerCondition->slot, definitions) ||
        !hasUnitUpdate(*innerExit, outerCondition->slot, definitions)) {
      continue;
    }

    std::vector<std::pair<std::string, std::string>> reductions;
    for (const auto& instruction : outerBody->instructions()) {
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3 ||
          instruction.operands()[2] == innerCondition->slot ||
          scalarSlots.find(instruction.operands()[2]) == scalarSlots.end()) {
        continue;
      }
      long long identity = 0;
      if (!detail::parseIntegerLiteral(instruction.operands()[1], identity) ||
          !reductionIsUpdated(*innerBody, instruction.operands()[2],
                              definitions)) {
        continue;
      }
      reductions.emplace_back(instruction.operands()[2],
                              instruction.operands()[1]);
    }
    if (reductions.size() != 1) {
      continue;
    }
    const auto reductionType = allocaType(reductions[0].first, definitions);
    const auto outerType = allocaType(outerCondition->slot, definitions);
    const auto destination =
        destinationAddress(*innerExit, outerCondition->slot,
                           reductions[0].first, scalarSlots, definitions);
    if (!reductionType.has_value() || !outerType.has_value() ||
        *reductionType != *outerType || !destination.has_value() ||
        !memoryDependencesPermitJamming(*innerBody, *destination,
                                        outerCondition->slot, scalarSlots,
                                        definitions)) {
      continue;
    }

    bool storesAreSupported = true;
    for (const auto& instruction : innerBody->instructions()) {
      if (instruction.opcode() == "store" &&
          !isStoreTo(instruction, innerCondition->slot) &&
          !isStoreTo(instruction, reductions[0].first)) {
        storesAreSupported = false;
      }
    }
    for (const auto& instruction : innerExit->instructions()) {
      if (instruction.opcode() == "store" &&
          !isStoreTo(instruction, outerCondition->slot) &&
          instruction.operands()[2] != *destination) {
        storesAreSupported = false;
      }
    }
    if (!storesAreSupported) {
      continue;
    }

    return Candidate{
        outerHeader.name(),    outerBodyName,         outerExitName,
        innerHeaderName,       innerBodyName,         innerExitName,
        outerCondition->slot,  innerCondition->slot,  reductions[0].first,
        outerCondition->value, outerCondition->bound, *reductionType,
        reductions[0].second, *destination};
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectValueNames(const Function& function) {
  std::unordered_set<std::string> names;
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
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshName(std::unordered_set<std::string>& names,
                      const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

struct PhiLoop {
  std::string body;
  std::string latch;
  std::string exit;
  std::string bound;
};

struct UniformSuffixPattern {
  std::string row;
  std::string destinationMatrix;
  std::string coefficientMatrix;
  std::int32_t fillValue = 0;
  std::int32_t thresholdDivisor = 0;
};

std::unordered_map<std::string, std::string>
definitionBlocks(const Function &function) {
  std::unordered_map<std::string, std::string> result;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        result.emplace(instruction.result(), block.name());
    }
  }
  return result;
}

bool isIntegerValue(const std::string &value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

std::optional<std::int32_t>
constantIntValue(const std::string &value, const DefinitionMap &definitions,
                 std::unordered_set<std::string> &visiting) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal)) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(literal));
  }
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return std::nullopt;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return std::nullopt;
  const auto &instruction = *found->second;
  if (instruction.opcode() == "neg" && instruction.operands().size() == 1) {
    auto operandVisiting = visiting;
    const auto operand = constantIntValue(instruction.operands()[0],
                                          definitions, operandVisiting);
    if (!operand.has_value())
      return std::nullopt;
    return static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(*operand));
  }
  if ((instruction.opcode() != "add" && instruction.opcode() != "sub" &&
       instruction.opcode() != "mul") ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }
  auto lhsVisiting = visiting;
  auto rhsVisiting = visiting;
  const auto lhs =
      constantIntValue(instruction.operands()[0], definitions, lhsVisiting);
  const auto rhs =
      constantIntValue(instruction.operands()[1], definitions, rhsVisiting);
  if (!lhs.has_value() || !rhs.has_value())
    return std::nullopt;
  const auto left = static_cast<std::uint32_t>(*lhs);
  const auto right = static_cast<std::uint32_t>(*rhs);
  std::uint32_t result = 0;
  if (instruction.opcode() == "add")
    result = left + right;
  if (instruction.opcode() == "sub")
    result = left - right;
  if (instruction.opcode() == "mul")
    result = left * right;
  return static_cast<std::int32_t>(result);
}

std::optional<std::int32_t> constantIntValue(const std::string &value,
                                             const DefinitionMap &definitions) {
  std::unordered_set<std::string> visiting;
  return constantIntValue(value, definitions, visiting);
}

std::string resolveEmptyForwarder(const Function &function, const CFG &cfg,
                                  std::string name) {
  std::unordered_set<std::string> visited;
  while (visited.insert(name).second) {
    const auto *block = findBlock(function, cfg, name);
    if (block == nullptr || block->instructions().size() != 1 ||
        block->terminator() == nullptr ||
        block->terminator()->opcode() != "br" ||
        block->terminator()->operands().size() != 1) {
      break;
    }
    name = block->terminator()->operands()[0];
  }
  return name;
}

bool canReachBlock(const Function &function, const CFG &cfg,
                   const std::string &start, const std::string &target) {
  const auto startFound = cfg.index.find(start);
  const auto targetFound = cfg.index.find(target);
  if (startFound == cfg.index.end() || targetFound == cfg.index.end()) {
    return false;
  }
  std::vector<bool> visited(function.blocks().size(), false);
  std::vector<std::size_t> worklist{startFound->second};
  while (!worklist.empty()) {
    const auto blockIndex = worklist.back();
    worklist.pop_back();
    if (blockIndex == targetFound->second)
      return true;
    if (visited[blockIndex])
      continue;
    visited[blockIndex] = true;
    const auto *terminator = function.blocks()[blockIndex].terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      const auto found = cfg.index.find(operands[index]);
      if (found != cfg.index.end() && !visited[found->second]) {
        worklist.push_back(found->second);
      }
    }
  }
  return false;
}

bool canReachBlockAvoiding(const Function &function, const CFG &cfg,
                           const std::string &start, const std::string &target,
                           const std::string &forbidden) {
  const auto startFound = cfg.index.find(start);
  const auto targetFound = cfg.index.find(target);
  const auto forbiddenFound = cfg.index.find(forbidden);
  if (startFound == cfg.index.end() || targetFound == cfg.index.end() ||
      forbiddenFound == cfg.index.end()) {
    return false;
  }
  std::vector<bool> visited(function.blocks().size(), false);
  std::vector<std::size_t> worklist{startFound->second};
  while (!worklist.empty()) {
    const auto blockIndex = worklist.back();
    worklist.pop_back();
    if (blockIndex == forbiddenFound->second &&
        blockIndex != targetFound->second) {
      continue;
    }
    if (blockIndex == targetFound->second)
      return true;
    if (visited[blockIndex])
      continue;
    visited[blockIndex] = true;
    const auto *terminator = function.blocks()[blockIndex].terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      const auto found = cfg.index.find(operands[index]);
      if (found != cfg.index.end() && !visited[found->second]) {
        worklist.push_back(found->second);
      }
    }
  }
  return false;
}

std::optional<PhiLoop> canonicalPhiLoop(
    const Function &function, const CFG &cfg, const DefinitionMap &definitions,
    const std::unordered_map<std::string, std::string> &definitionBlock,
    const std::string &induction, const std::string &expectedBound) {
  const auto phiFound = definitions.find(induction);
  const auto headerFound = definitionBlock.find(induction);
  if (phiFound == definitions.end() || headerFound == definitionBlock.end() ||
      phiFound->second->opcode() != "phi" ||
      phiFound->second->operands().size() != 5) {
    return std::nullopt;
  }
  const auto &operands = phiFound->second->operands();
  std::string latch;
  std::string next;
  if (isIntegerValue(operands[1], 0)) {
    next = operands[3];
    latch = operands[4];
  } else if (isIntegerValue(operands[3], 0)) {
    next = operands[1];
    latch = operands[2];
  } else {
    return std::nullopt;
  }
  const auto nextFound = definitions.find(next);
  const auto nextBlock = definitionBlock.find(next);
  if (nextFound == definitions.end() || nextBlock == definitionBlock.end() ||
      nextBlock->second != latch || nextFound->second->opcode() != "add" ||
      nextFound->second->operands().size() != 2 ||
      !((nextFound->second->operands()[0] == induction &&
         isIntegerValue(nextFound->second->operands()[1], 1)) ||
        (nextFound->second->operands()[1] == induction &&
         isIntegerValue(nextFound->second->operands()[0], 1)))) {
    return std::nullopt;
  }
  const auto *header = findBlock(function, cfg, headerFound->second);
  const auto *latchBlock = findBlock(function, cfg, latch);
  if (header == nullptr || latchBlock == nullptr ||
      header->terminator() == nullptr || latchBlock->terminator() == nullptr ||
      header->terminator()->opcode() != "br" ||
      header->terminator()->operands().size() != 3 ||
      latchBlock->terminator()->opcode() != "br" ||
      latchBlock->terminator()->operands() !=
          std::vector<std::string>{header->name()}) {
    return std::nullopt;
  }
  const auto condition = definitions.find(header->terminator()->operands()[0]);
  if (condition == definitions.end() ||
      condition->second->opcode() != "icmp.slt" ||
      condition->second->operands().size() != 2 ||
      condition->second->operands()[0] != induction ||
      condition->second->operands()[1] != expectedBound) {
    return std::nullopt;
  }
  return PhiLoop{header->terminator()->operands()[1], latch,
                 header->terminator()->operands()[2], expectedBound};
}

std::optional<UniformSuffixPattern>
findUniformSuffixPattern(const Function &function, const Candidate &candidate) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto scalarSlots = collectScalarSlots(function);
  const auto definitionBlock = definitionBlocks(function);
  const auto destination = pointerPath(candidate.destination, definitions);
  const auto *innerBody = findBlock(function, cfg, candidate.innerBody);
  if (!destination.has_value() || destination->indices.size() != 2 ||
      innerBody == nullptr ||
      !isValueLoadedFromSlot(destination->indices[1], candidate.outerSlot,
                             definitions)) {
    return std::nullopt;
  }
  const std::string row = destination->indices[0];

  struct LoadPath {
    std::string result;
    PointerPath path;
  };
  std::vector<LoadPath> arrayLoads;
  for (const auto &instruction : innerBody->instructions()) {
    if (instruction.opcode() == "call")
      return std::nullopt;
    if (instruction.opcode() == "store" &&
        !isStoreTo(instruction, candidate.innerSlot) &&
        !isStoreTo(instruction, candidate.reductionSlot)) {
      return std::nullopt;
    }
    if (instruction.opcode() != "load" || instruction.operands().size() != 2 ||
        scalarSlots.find(instruction.operands()[1]) != scalarSlots.end()) {
      continue;
    }
    const auto path = pointerPath(instruction.operands()[1], definitions);
    if (!path.has_value() || path->indices.size() != 2)
      return std::nullopt;
    arrayLoads.push_back({instruction.result(), *path});
  }
  if (arrayLoads.size() != 2)
    return std::nullopt;
  const LoadPath *sourceLoad = nullptr;
  const LoadPath *coefficientLoad = nullptr;
  for (const auto &load : arrayLoads) {
    if (load.path.base == destination->base &&
        isValueLoadedFromSlot(load.path.indices[0], candidate.innerSlot,
                              definitions) &&
        isValueLoadedFromSlot(load.path.indices[1], candidate.outerSlot,
                              definitions)) {
      sourceLoad = &load;
    } else if (load.path.base != destination->base &&
               load.path.indices[0] == row &&
               isValueLoadedFromSlot(load.path.indices[1], candidate.innerSlot,
                                     definitions)) {
      coefficientLoad = &load;
    }
  }
  if (sourceLoad == nullptr || coefficientLoad == nullptr)
    return std::nullopt;

  const auto accumulator =
      reductionLoadResult(*innerBody, candidate.reductionSlot);
  const Instruction *product = nullptr;
  const Instruction *sum = nullptr;
  if (!accumulator.has_value())
    return std::nullopt;
  for (const auto &instruction : innerBody->instructions()) {
    if (instruction.opcode() == "mul" && instruction.operands().size() == 2 &&
        ((instruction.operands()[0] == sourceLoad->result &&
          instruction.operands()[1] == coefficientLoad->result) ||
         (instruction.operands()[1] == sourceLoad->result &&
          instruction.operands()[0] == coefficientLoad->result))) {
      if (product != nullptr)
        return std::nullopt;
      product = &instruction;
    }
  }
  if (product == nullptr)
    return std::nullopt;
  for (const auto &instruction : innerBody->instructions()) {
    if (instruction.opcode() == "add" && instruction.operands().size() == 2 &&
        ((instruction.operands()[0] == *accumulator &&
          instruction.operands()[1] == product->result()) ||
         (instruction.operands()[1] == *accumulator &&
          instruction.operands()[0] == product->result()))) {
      if (sum != nullptr)
        return std::nullopt;
      sum = &instruction;
    }
  }
  bool storesSum = false;
  if (sum == nullptr)
    return std::nullopt;
  for (const auto &instruction : innerBody->instructions()) {
    if (isStoreTo(instruction, candidate.reductionSlot) &&
        instruction.operands()[1] == sum->result()) {
      storesSum = true;
    }
  }
  if (!storesSum)
    return std::nullopt;

  const auto candidateHeader = cfg.index.find(candidate.outerHeader);
  if (candidateHeader == cfg.index.end())
    return std::nullopt;
  std::string candidatePreheader;
  for (const auto &predecessor : cfg.predecessors[candidateHeader->second]) {
    if (predecessor != candidate.innerExit)
      candidatePreheader = predecessor;
  }
  const auto rowLoop = canonicalPhiLoop(
      function, cfg, definitions, definitionBlock, row, candidate.outerBound);
  if (candidatePreheader.empty() || !rowLoop.has_value() ||
      rowLoop->body != candidatePreheader) {
    return std::nullopt;
  }

  for (const auto &block : function.blocks()) {
    for (const auto &store : block.instructions()) {
      if (store.opcode() != "store" || store.operands().size() != 3)
        continue;
      const auto path = pointerPath(store.operands()[2], definitions);
      if (!path.has_value() || path->base != destination->base ||
          path->indices.size() != 2) {
        continue;
      }
      const auto fillValue = constantIntValue(store.operands()[1], definitions);
      const auto columnLoad = definitions.find(path->indices[1]);
      if (!fillValue.has_value() || columnLoad == definitions.end() ||
          columnLoad->second->opcode() != "load" ||
          columnLoad->second->operands().size() != 2 ||
          block.terminator() == nullptr ||
          block.terminator()->opcode() != "br" ||
          block.terminator()->operands().size() != 1) {
        continue;
      }
      const std::string columnSlot = columnLoad->second->operands()[1];
      if (!hasUnitUpdate(block, columnSlot, definitions))
        continue;
      const std::string columnHeaderName = block.terminator()->operands()[0];
      const auto *columnHeader = findBlock(function, cfg, columnHeaderName);
      const auto columnCondition =
          columnHeader == nullptr ? std::optional<LoopCondition>{}
                                  : loopCondition(*columnHeader, definitions);
      if (!columnCondition.has_value() || columnCondition->slot != columnSlot ||
          columnCondition->bound != candidate.outerBound ||
          columnHeader->terminator()->operands()[1] != block.name()) {
        continue;
      }
      const std::string outerLatch = resolveEmptyForwarder(
          function, cfg, columnHeader->terminator()->operands()[2]);
      const auto columnHeaderIndex = cfg.index.find(columnHeaderName);
      if (columnHeaderIndex == cfg.index.end() ||
          cfg.predecessors[columnHeaderIndex->second].size() != 2) {
        continue;
      }
      std::string columnPreheader;
      for (const auto &predecessor :
           cfg.predecessors[columnHeaderIndex->second]) {
        if (predecessor != block.name())
          columnPreheader = predecessor;
      }
      const auto *setup = findBlock(function, cfg, columnPreheader);
      const auto setupIndex = cfg.index.find(columnPreheader);
      if (setup == nullptr || setupIndex == cfg.index.end() ||
          !hasConstantStore(*setup, columnSlot, "0") ||
          cfg.predecessors[setupIndex->second].size() != 1) {
        continue;
      }
      const std::string fillBranchName =
          cfg.predecessors[setupIndex->second].front();
      const auto *fillBranch = findBlock(function, cfg, fillBranchName);
      if (fillBranch == nullptr || fillBranch->terminator() == nullptr ||
          fillBranch->terminator()->opcode() != "br" ||
          fillBranch->terminator()->operands().size() != 3) {
        continue;
      }
      const auto &branch = fillBranch->terminator()->operands();
      const bool fillsOnTrue = branch[1] == columnPreheader;
      if ((!fillsOnTrue && branch[2] != columnPreheader) ||
          resolveEmptyForwarder(function, cfg,
                                fillsOnTrue ? branch[2] : branch[1]) !=
              outerLatch) {
        continue;
      }
      const auto comparison = definitions.find(branch[0]);
      if (comparison == definitions.end() ||
          comparison->second->operands().size() != 2) {
        continue;
      }
      const std::string fillRow = path->indices[0];
      std::string threshold;
      if (fillsOnTrue && comparison->second->opcode() == "icmp.sge" &&
          comparison->second->operands()[0] == fillRow) {
        threshold = comparison->second->operands()[1];
      } else if (!fillsOnTrue && comparison->second->opcode() == "icmp.slt" &&
                 comparison->second->operands()[0] == fillRow) {
        threshold = comparison->second->operands()[1];
      } else {
        continue;
      }
      const auto thresholdDefinition = definitions.find(threshold);
      if (thresholdDefinition == definitions.end() ||
          thresholdDefinition->second->opcode() != "sdiv" ||
          thresholdDefinition->second->operands().size() != 2 ||
          thresholdDefinition->second->operands()[0] != candidate.outerBound) {
        continue;
      }
      long long divisor = 0;
      if (!detail::parseIntegerLiteral(
              thresholdDefinition->second->operands()[1], divisor) ||
          divisor <= 0 || divisor > INT32_MAX) {
        continue;
      }
      const auto fillRowLoop =
          canonicalPhiLoop(function, cfg, definitions, definitionBlock, fillRow,
                           candidate.outerBound);
      if (!fillRowLoop.has_value() || fillRowLoop->body != fillBranchName ||
          fillRowLoop->latch != outerLatch ||
          valueDependsOn(store.operands()[1], fillRow, definitions) ||
          valueDependsOn(store.operands()[1], path->indices[1], definitions)) {
        continue;
      }
      const std::string rawFillExit = fillRowLoop->exit;
      const std::string fillExit =
          resolveEmptyForwarder(function, cfg, rawFillExit);
      if (!canReachBlock(function, cfg, fillExit, candidatePreheader) ||
          canReachBlockAvoiding(function, cfg, function.blocks().front().name(),
                                candidatePreheader, rawFillExit)) {
        continue;
      }
      bool clobbered = false;
      for (std::size_t index = 0;
           index < function.blocks().size() && !clobbered; ++index) {
        const auto &name = function.blocks()[index].name();
        if (name == candidatePreheader ||
            !canReachBlockAvoiding(function, cfg, fillExit, name,
                                   candidatePreheader) ||
            !canReachBlock(function, cfg, name, candidatePreheader)) {
          continue;
        }
        for (const auto &instruction :
             function.blocks()[index].instructions()) {
          if (instruction.opcode() == "call") {
            clobbered = true;
            break;
          }
          if (instruction.opcode() != "store" ||
              instruction.operands().size() != 3) {
            continue;
          }
          if (scalarSlots.find(instruction.operands()[2]) !=
              scalarSlots.end()) {
            continue;
          }
          const auto storedPath =
              pointerPath(instruction.operands()[2], definitions);
          if (!storedPath.has_value() ||
              storedPath->base == destination->base) {
            clobbered = true;
            break;
          }
        }
      }
      if (!clobbered) {
        return UniformSuffixPattern{row, destination->base,
                                    coefficientLoad->path.base, *fillValue,
                                    static_cast<std::int32_t>(divisor)};
      }
    }
  }
  return std::nullopt;
}

bool foldUniformSuffixContribution(Function &function, Candidate &candidate) {
  if (std::getenv("COMPILER_DISABLE_UNIFORM_SUFFIX_CONTRIBUTION_FOLDING") !=
      nullptr) {
    return false;
  }
  const auto pattern = findUniformSuffixPattern(function, candidate);
  if (!pattern.has_value())
    return false;
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto headerIndex = cfg.index.find(candidate.outerHeader);
  if (headerIndex == cfg.index.end())
    return false;
  std::string preheaderName;
  for (const auto &predecessor : cfg.predecessors[headerIndex->second]) {
    if (predecessor != candidate.innerExit)
      preheaderName = predecessor;
  }
  auto *preheader = findBlock(function, preheaderName);
  auto *outerBody = findBlock(function, candidate.outerBody);
  auto *innerHeader = findBlock(function, candidate.innerHeader);
  if (preheader == nullptr || outerBody == nullptr || innerHeader == nullptr ||
      preheader->terminator() == nullptr ||
      preheader->terminator()->operands() !=
          std::vector<std::string>{candidate.outerHeader}) {
    return false;
  }
  std::optional<std::size_t> identityStore;
  for (std::size_t index = 0; index < outerBody->instructions().size();
       ++index) {
    const auto &instruction = outerBody->instructions()[index];
    if (isStoreTo(instruction, candidate.reductionSlot) &&
        instruction.operands()[1] == candidate.reductionIdentity) {
      if (identityStore.has_value())
        return false;
      identityStore = index;
    }
  }
  const auto innerCondition = loopCondition(*innerHeader, definitions);
  std::optional<std::size_t> innerCompare;
  if (innerCondition.has_value()) {
    for (std::size_t index = 0; index < innerHeader->instructions().size();
         ++index) {
      const auto &instruction = innerHeader->instructions()[index];
      if (instruction.opcode() == "icmp.slt" &&
          instruction.result() == innerHeader->terminator()->operands()[0] &&
          instruction.operands().size() == 2 &&
          instruction.operands()[1] == innerCondition->bound) {
        innerCompare = index;
      }
    }
  }
  if (!identityStore.has_value() || !innerCompare.has_value())
    return false;

  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto threshold = freshName(valueNames, "%uniform.slice.threshold");
  const auto below = freshName(valueNames, "%uniform.slice.below");
  const auto cutoff = freshName(valueNames, "%uniform.slice.cutoff");
  const auto suffixHeader = freshName(blockNames, "uniform.slice.header");
  const auto suffixBody = freshName(blockNames, "uniform.slice.body");
  const auto suffixK = freshName(valueNames, "%uniform.slice.k");
  const auto suffixAcc = freshName(valueNames, "%uniform.slice.acc");
  const auto hasK = freshName(valueNames, "%uniform.slice.has.k");
  const auto coefficientRow = freshName(valueNames, "%uniform.slice.row");
  const auto coefficientAddress =
      freshName(valueNames, "%uniform.slice.address");
  const auto coefficient = freshName(valueNames, "%uniform.slice.coefficient");
  const auto product = freshName(valueNames, "%uniform.slice.product");
  const auto nextAcc = freshName(valueNames, "%uniform.slice.acc.next");
  const auto nextK = freshName(valueNames, "%uniform.slice.k.next");

  auto &preheaderCode = preheader->instructions();
  auto insertion = preheaderCode.end() - 1;
  insertion = preheaderCode.insert(
      insertion, Instruction{"sdiv",
                             {candidate.outerBound,
                              std::to_string(pattern->thresholdDivisor)},
                             threshold});
  ++insertion;
  insertion = preheaderCode.insert(
      insertion, Instruction{"icmp.slt", {pattern->row, threshold}, below});
  ++insertion;
  insertion = preheaderCode.insert(
      insertion,
      Instruction{"select", {below, threshold, pattern->row}, cutoff});
  ++insertion;
  preheaderCode.insert(insertion,
                       Instruction{"getelementptr",
                                   {pattern->coefficientMatrix, pattern->row},
                                   coefficientRow});
  preheaderCode.back() = Instruction{"br", {suffixHeader}};

  BasicBlock header{suffixHeader};
  header.addInstruction(Instruction{
      "phi", {"int", cutoff, preheaderName, nextK, suffixBody}, suffixK});
  header.addInstruction(Instruction{
      "phi", {"int", "0", preheaderName, nextAcc, suffixBody}, suffixAcc});
  header.addInstruction(
      Instruction{"icmp.slt", {suffixK, candidate.outerBound}, hasK});
  header.addInstruction(
      Instruction{"br", {hasK, suffixBody, candidate.outerHeader}});
  BasicBlock body{suffixBody};
  body.addInstruction(Instruction{
      "getelementptr", {coefficientRow, suffixK}, coefficientAddress});
  body.addInstruction(
      Instruction{"load", {"int", coefficientAddress}, coefficient});
  body.addInstruction(Instruction{
      "mul", {coefficient, std::to_string(pattern->fillValue)}, product});
  body.addInstruction(Instruction{"add", {suffixAcc, product}, nextAcc});
  body.addInstruction(Instruction{"add", {suffixK, "1"}, nextK});
  body.addInstruction(Instruction{"br", {suffixHeader}});

  auto identityOperands = outerBody->instructions()[*identityStore].operands();
  identityOperands[1] = suffixAcc;
  outerBody->instructions()[*identityStore] =
      Instruction{"store", std::move(identityOperands)};
  auto compareOperands = innerHeader->instructions()[*innerCompare].operands();
  compareOperands[1] = cutoff;
  const auto compareResult =
      innerHeader->instructions()[*innerCompare].result();
  innerHeader->instructions()[*innerCompare] =
      Instruction{"icmp.slt", std::move(compareOperands), compareResult};
  candidate.reductionIdentity = suffixAcc;
  function.addBlock(std::move(header));
  function.addBlock(std::move(body));
  return true;
}

bool buildOrderedTwoRowPanel(Function &function, const Candidate &candidate,
                             const UniformSuffixPattern &pattern) {
  if (std::getenv("COMPILER_DISABLE_ORDERED_TWO_ROW_PANEL") != nullptr) {
    return false;
  }
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto definitionBlock = definitionBlocks(function);
  const auto rowDefinition = definitions.find(pattern.row);
  const auto rowBlock = definitionBlock.find(pattern.row);
  if (rowDefinition == definitions.end() ||
      rowDefinition->second->opcode() != "phi" ||
      rowDefinition->second->operands().size() != 5 ||
      rowBlock == definitionBlock.end()) {
    return false;
  }
  auto *rowHeader = findBlock(function, rowBlock->second);
  const auto rowLoop = canonicalPhiLoop(
      function, cfg, definitions, definitionBlock, pattern.row,
      candidate.outerBound);
  if (rowHeader == nullptr || !rowLoop.has_value() ||
      rowHeader->terminator() == nullptr ||
      rowHeader->terminator()->opcode() != "br" ||
      rowHeader->terminator()->operands().size() != 3 ||
      rowHeader->terminator()->operands()[1] != rowLoop->body) {
    return false;
  }

  // The original scalar/32-column path remains the complete fallback.  The
  // panel path is entered only when every column belongs to a full 16-wide
  // block and a second row is available.
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  auto value = [&](const std::string &stem) {
    return freshName(valueNames, "%ordered.panel." + stem);
  };
  auto block = [&](const std::string &stem) {
    return freshName(blockNames, "ordered.panel." + stem);
  };

  const auto row1 = value("row1");
  const auto hasRow1 = value("has.row1");
  const auto columnRemainder = value("column.remainder");
  const auto fullColumns = value("full.columns");
  const auto usePanel = value("use");
  const auto guardName = block("guard");
  const auto setupName = block("setup");
  const auto suffix0HeaderName = block("suffix0.header");
  const auto suffix0BodyName = block("suffix0.body");
  const auto suffix1HeaderName = block("suffix1.header");
  const auto suffix1BodyName = block("suffix1.body");
  const auto columnHeaderName = block("column.header");
  const auto columnBodyName = block("column.body");
  const auto prefixHeaderName = block("prefix.header");
  const auto prefixBodyName = block("prefix.body");
  const auto highHeaderName = block("high.header");
  const auto highBodyName = block("high.body");
  const auto old0GuardName = block("old0.guard");
  const auto old0BodyName = block("old0.body");
  const auto old1Row0GuardName = block("old1.row0.guard");
  const auto old1Row0BodyName = block("old1.row0.body");
  const auto new0ToRow1Name = block("new0.to.row1");
  const auto old1Row1GuardName = block("old1.row1.guard");
  const auto old1Row1BodyName = block("old1.row1.body");
  const auto storeName = block("store");
  const auto rowLatchName = block("row.latch");

  const auto threshold = value("threshold");
  const auto below0 = value("below0");
  const auto below1 = value("below1");
  const auto cutoff0 = value("cutoff0");
  const auto cutoff1 = value("cutoff1");
  const auto coefficientRow0 = value("coefficient.row0");
  const auto coefficientRow1 = value("coefficient.row1");
  const auto destinationRow0 = value("destination.row0");
  const auto destinationRow1 = value("destination.row1");

  const auto suffix0K = value("suffix0.k");
  const auto suffix0Acc = value("suffix0.acc");
  const auto suffix0HasK = value("suffix0.has.k");
  const auto suffix0Address = value("suffix0.address");
  const auto suffix0Coefficient = value("suffix0.coefficient");
  const auto suffix0Product = value("suffix0.product");
  const auto suffix0NextAcc = value("suffix0.acc.next");
  const auto suffix0NextK = value("suffix0.k.next");
  const auto suffix1K = value("suffix1.k");
  const auto suffix1Acc = value("suffix1.acc");
  const auto suffix1HasK = value("suffix1.has.k");
  const auto suffix1Address = value("suffix1.address");
  const auto suffix1Coefficient = value("suffix1.coefficient");
  const auto suffix1Product = value("suffix1.product");
  const auto suffix1NextAcc = value("suffix1.acc.next");
  const auto suffix1NextK = value("suffix1.k.next");

  const auto column = value("column");
  const auto hasColumn = value("has.column");
  const auto destination0 = value("destination0");
  const auto destination1 = value("destination1");
  const auto prefixK = value("prefix.k");
  const auto prefixHasK = value("prefix.has.k");
  const auto prefixSourceRow = value("prefix.source.row");
  const auto prefixSource = value("prefix.source");
  const auto prefixCoefficient0Address = value("prefix.coefficient0.address");
  const auto prefixCoefficient1Address = value("prefix.coefficient1.address");
  const auto prefixCoefficient0 = value("prefix.coefficient0");
  const auto prefixCoefficient1 = value("prefix.coefficient1");
  const auto prefixNextK = value("prefix.k.next");
  const auto highStart = value("high.start");
  const auto highK = value("high.k");
  const auto highHasK = value("high.has.k");
  const auto highSourceRow = value("high.source.row");
  const auto highSource = value("high.source");
  const auto highCoefficient0Address = value("high.coefficient0.address");
  const auto highCoefficient1Address = value("high.coefficient1.address");
  const auto highCoefficient0 = value("high.coefficient0");
  const auto highCoefficient1 = value("high.coefficient1");
  const auto highNextK = value("high.k.next");
  const auto includeOld0 = value("include.old0");
  const auto coefficient00Address = value("coefficient00.address");
  const auto coefficient00 = value("coefficient00");
  const auto includeOld1Row0 = value("include.old1.row0");
  const auto coefficient01Address = value("coefficient01.address");
  const auto coefficient01 = value("coefficient01");
  const auto coefficient10Address = value("coefficient10.address");
  const auto coefficient10 = value("coefficient10");
  const auto includeOld1Row1 = value("include.old1.row1");
  const auto coefficient11Address = value("coefficient11.address");
  const auto coefficient11 = value("coefficient11");
  const auto nextColumn = value("column.next");
  const auto nextRow = value("row.next");

  auto rowTerminator = rowHeader->terminator()->operands();
  rowTerminator[1] = guardName;
  rowHeader->instructions().back() =
      Instruction{"br", std::move(rowTerminator)};

  BasicBlock guard{guardName};
  guard.addInstruction(Instruction{"add", {pattern.row, "1"}, row1});
  guard.addInstruction(
      Instruction{"icmp.slt", {row1, candidate.outerBound}, hasRow1});
  guard.addInstruction(
      Instruction{"srem", {candidate.outerBound, "16"}, columnRemainder});
  guard.addInstruction(
      Instruction{"icmp.eq", {columnRemainder, "0"}, fullColumns});
  guard.addInstruction(Instruction{"and", {hasRow1, fullColumns}, usePanel});
  guard.addInstruction(
      Instruction{"br", {usePanel, setupName, rowLoop->body}});

  BasicBlock setup{setupName};
  setup.addInstruction(Instruction{
      "sdiv",
      {candidate.outerBound, std::to_string(pattern.thresholdDivisor)},
      threshold});
  setup.addInstruction(Instruction{"icmp.slt", {pattern.row, threshold}, below0});
  setup.addInstruction(Instruction{"icmp.slt", {row1, threshold}, below1});
  setup.addInstruction(
      Instruction{"select", {below0, threshold, pattern.row}, cutoff0});
  setup.addInstruction(
      Instruction{"select", {below1, threshold, row1}, cutoff1});
  setup.addInstruction(Instruction{"getelementptr",
                                   {pattern.coefficientMatrix, pattern.row},
                                   coefficientRow0});
  setup.addInstruction(Instruction{"getelementptr",
                                   {pattern.coefficientMatrix, row1},
                                   coefficientRow1});
  setup.addInstruction(Instruction{"getelementptr",
                                   {pattern.destinationMatrix, pattern.row},
                                   destinationRow0});
  setup.addInstruction(Instruction{"getelementptr",
                                   {pattern.destinationMatrix, row1},
                                   destinationRow1});
  setup.addInstruction(Instruction{"br", {suffix0HeaderName}});

  BasicBlock suffix0Header{suffix0HeaderName};
  suffix0Header.addInstruction(
      Instruction{"phi",
                  {"int", cutoff0, setupName, suffix0NextK, suffix0BodyName},
                  suffix0K});
  suffix0Header.addInstruction(
      Instruction{"phi",
                  {"int", "0", setupName, suffix0NextAcc, suffix0BodyName},
                  suffix0Acc});
  suffix0Header.addInstruction(
      Instruction{"icmp.slt", {suffix0K, candidate.outerBound}, suffix0HasK});
  suffix0Header.addInstruction(
      Instruction{"br",
                  {suffix0HasK, suffix0BodyName, suffix1HeaderName}});

  BasicBlock suffix0Body{suffix0BodyName};
  suffix0Body.addInstruction(Instruction{
      "getelementptr", {coefficientRow0, suffix0K}, suffix0Address});
  suffix0Body.addInstruction(
      Instruction{"load", {"int", suffix0Address}, suffix0Coefficient});
  suffix0Body.addInstruction(
      Instruction{"mul",
                  {suffix0Coefficient, std::to_string(pattern.fillValue)},
                  suffix0Product});
  suffix0Body.addInstruction(
      Instruction{"add", {suffix0Acc, suffix0Product}, suffix0NextAcc});
  suffix0Body.addInstruction(
      Instruction{"add", {suffix0K, "1"}, suffix0NextK});
  suffix0Body.addInstruction(Instruction{"br", {suffix0HeaderName}});

  BasicBlock suffix1Header{suffix1HeaderName};
  suffix1Header.addInstruction(
      Instruction{"phi",
                  {"int", cutoff1, suffix0HeaderName, suffix1NextK,
                   suffix1BodyName},
                  suffix1K});
  suffix1Header.addInstruction(
      Instruction{"phi",
                  {"int", "0", suffix0HeaderName, suffix1NextAcc,
                   suffix1BodyName},
                  suffix1Acc});
  suffix1Header.addInstruction(
      Instruction{"icmp.slt", {suffix1K, candidate.outerBound}, suffix1HasK});
  suffix1Header.addInstruction(
      Instruction{"br",
                  {suffix1HasK, suffix1BodyName, columnHeaderName}});

  BasicBlock suffix1Body{suffix1BodyName};
  suffix1Body.addInstruction(Instruction{
      "getelementptr", {coefficientRow1, suffix1K}, suffix1Address});
  suffix1Body.addInstruction(
      Instruction{"load", {"int", suffix1Address}, suffix1Coefficient});
  suffix1Body.addInstruction(
      Instruction{"mul",
                  {suffix1Coefficient, std::to_string(pattern.fillValue)},
                  suffix1Product});
  suffix1Body.addInstruction(
      Instruction{"add", {suffix1Acc, suffix1Product}, suffix1NextAcc});
  suffix1Body.addInstruction(
      Instruction{"add", {suffix1K, "1"}, suffix1NextK});
  suffix1Body.addInstruction(Instruction{"br", {suffix1HeaderName}});

  BasicBlock columnHeader{columnHeaderName};
  columnHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", suffix1HeaderName, nextColumn, storeName},
                  column});
  columnHeader.addInstruction(
      Instruction{"icmp.slt", {column, candidate.outerBound}, hasColumn});
  columnHeader.addInstruction(
      Instruction{"br", {hasColumn, columnBodyName, rowLatchName}});

  BasicBlock columnBody{columnBodyName};
  columnBody.addInstruction(
      Instruction{"getelementptr", {destinationRow0, column}, destination0});
  columnBody.addInstruction(
      Instruction{"getelementptr", {destinationRow1, column}, destination1});
  columnBody.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.init",
      {destination0, destination1, suffix0Acc, suffix1Acc}});
  columnBody.addInstruction(
      Instruction{"add", {pattern.row, "2"}, highStart});
  columnBody.addInstruction(Instruction{"br", {prefixHeaderName}});

  BasicBlock prefixHeader{prefixHeaderName};
  prefixHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", columnBodyName, prefixNextK, prefixBodyName},
                  prefixK});
  prefixHeader.addInstruction(
      Instruction{"icmp.slt", {prefixK, pattern.row}, prefixHasK});
  prefixHeader.addInstruction(
      Instruction{"br", {prefixHasK, prefixBodyName, highHeaderName}});

  BasicBlock prefixBody{prefixBodyName};
  prefixBody.addInstruction(Instruction{
      "getelementptr", {pattern.destinationMatrix, prefixK}, prefixSourceRow});
  prefixBody.addInstruction(
      Instruction{"getelementptr", {prefixSourceRow, column}, prefixSource});
  prefixBody.addInstruction(Instruction{
      "getelementptr", {coefficientRow0, prefixK},
      prefixCoefficient0Address});
  prefixBody.addInstruction(Instruction{
      "getelementptr", {coefficientRow1, prefixK},
      prefixCoefficient1Address});
  prefixBody.addInstruction(Instruction{
      "load", {"int", prefixCoefficient0Address}, prefixCoefficient0});
  prefixBody.addInstruction(Instruction{
      "load", {"int", prefixCoefficient1Address}, prefixCoefficient1});
  prefixBody.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.shared",
      {prefixSource, prefixCoefficient0, prefixCoefficient1}});
  prefixBody.addInstruction(
      Instruction{"add", {prefixK, "1"}, prefixNextK});
  prefixBody.addInstruction(Instruction{"br", {prefixHeaderName}});

  BasicBlock highHeader{highHeaderName};
  highHeader.addInstruction(
      Instruction{"phi",
                  {"int", highStart, prefixHeaderName, highNextK, highBodyName},
                  highK});
  highHeader.addInstruction(
      Instruction{"icmp.slt", {highK, cutoff0}, highHasK});
  highHeader.addInstruction(
      Instruction{"br", {highHasK, highBodyName, old0GuardName}});

  BasicBlock highBody{highBodyName};
  highBody.addInstruction(Instruction{
      "getelementptr", {pattern.destinationMatrix, highK}, highSourceRow});
  highBody.addInstruction(
      Instruction{"getelementptr", {highSourceRow, column}, highSource});
  highBody.addInstruction(Instruction{
      "getelementptr", {coefficientRow0, highK}, highCoefficient0Address});
  highBody.addInstruction(Instruction{
      "getelementptr", {coefficientRow1, highK}, highCoefficient1Address});
  highBody.addInstruction(
      Instruction{"load", {"int", highCoefficient0Address}, highCoefficient0});
  highBody.addInstruction(
      Instruction{"load", {"int", highCoefficient1Address}, highCoefficient1});
  highBody.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.shared",
      {highSource, highCoefficient0, highCoefficient1}});
  highBody.addInstruction(Instruction{"add", {highK, "1"}, highNextK});
  highBody.addInstruction(Instruction{"br", {highHeaderName}});

  BasicBlock old0Guard{old0GuardName};
  old0Guard.addInstruction(
      Instruction{"icmp.slt", {pattern.row, cutoff0}, includeOld0});
  old0Guard.addInstruction(
      Instruction{"br", {includeOld0, old0BodyName, old1Row0GuardName}});

  BasicBlock old0Body{old0BodyName};
  old0Body.addInstruction(Instruction{
      "getelementptr", {coefficientRow0, pattern.row}, coefficient00Address});
  old0Body.addInstruction(
      Instruction{"load", {"int", coefficient00Address}, coefficient00});
  old0Body.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.old0.to.row0", {coefficient00}});
  old0Body.addInstruction(Instruction{"br", {old1Row0GuardName}});

  BasicBlock old1Row0Guard{old1Row0GuardName};
  old1Row0Guard.addInstruction(
      Instruction{"icmp.slt", {row1, cutoff0}, includeOld1Row0});
  old1Row0Guard.addInstruction(Instruction{
      "br", {includeOld1Row0, old1Row0BodyName, new0ToRow1Name}});

  BasicBlock old1Row0Body{old1Row0BodyName};
  old1Row0Body.addInstruction(
      Instruction{"getelementptr", {coefficientRow0, row1},
                  coefficient01Address});
  old1Row0Body.addInstruction(
      Instruction{"load", {"int", coefficient01Address}, coefficient01});
  old1Row0Body.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.old1.to.row0", {coefficient01}});
  old1Row0Body.addInstruction(Instruction{"br", {new0ToRow1Name}});

  BasicBlock new0ToRow1{new0ToRow1Name};
  new0ToRow1.addInstruction(
      Instruction{"getelementptr", {coefficientRow1, pattern.row},
                  coefficient10Address});
  new0ToRow1.addInstruction(
      Instruction{"load", {"int", coefficient10Address}, coefficient10});
  new0ToRow1.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.new0.to.row1", {coefficient10}});
  new0ToRow1.addInstruction(Instruction{"br", {old1Row1GuardName}});

  BasicBlock old1Row1Guard{old1Row1GuardName};
  old1Row1Guard.addInstruction(
      Instruction{"icmp.slt", {row1, cutoff1}, includeOld1Row1});
  old1Row1Guard.addInstruction(
      Instruction{"br", {includeOld1Row1, old1Row1BodyName, storeName}});

  BasicBlock old1Row1Body{old1Row1BodyName};
  old1Row1Body.addInstruction(
      Instruction{"getelementptr", {coefficientRow1, row1},
                  coefficient11Address});
  old1Row1Body.addInstruction(
      Instruction{"load", {"int", coefficient11Address}, coefficient11});
  old1Row1Body.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.old1.to.row1", {coefficient11}});
  old1Row1Body.addInstruction(Instruction{"br", {storeName}});

  BasicBlock store{storeName};
  store.addInstruction(Instruction{
      "aarch64.neon.ordered.panel.i32x16.store",
      {destination0, destination1}});
  store.addInstruction(Instruction{"add", {column, "16"}, nextColumn});
  store.addInstruction(Instruction{"br", {columnHeaderName}});

  BasicBlock rowLatch{rowLatchName};
  rowLatch.addInstruction(Instruction{"add", {pattern.row, "2"}, nextRow});
  rowLatch.addInstruction(Instruction{"br", {rowBlock->second}});

  auto rowPhiOperands = rowDefinition->second->operands();
  rowPhiOperands.push_back(nextRow);
  rowPhiOperands.push_back(rowLatchName);
  for (auto &instruction : rowHeader->instructions()) {
    if (instruction.result() == pattern.row && instruction.opcode() == "phi") {
      instruction =
          Instruction{"phi", std::move(rowPhiOperands), pattern.row};
      break;
    }
  }

  function.addBlock(std::move(guard));
  function.addBlock(std::move(setup));
  function.addBlock(std::move(suffix0Header));
  function.addBlock(std::move(suffix0Body));
  function.addBlock(std::move(suffix1Header));
  function.addBlock(std::move(suffix1Body));
  function.addBlock(std::move(columnHeader));
  function.addBlock(std::move(columnBody));
  function.addBlock(std::move(prefixHeader));
  function.addBlock(std::move(prefixBody));
  function.addBlock(std::move(highHeader));
  function.addBlock(std::move(highBody));
  function.addBlock(std::move(old0Guard));
  function.addBlock(std::move(old0Body));
  function.addBlock(std::move(old1Row0Guard));
  function.addBlock(std::move(old1Row0Body));
  function.addBlock(std::move(new0ToRow1));
  function.addBlock(std::move(old1Row1Guard));
  function.addBlock(std::move(old1Row1Body));
  function.addBlock(std::move(store));
  function.addBlock(std::move(rowLatch));
  return true;
}

bool buildOrderedEightRowPanel8Lazy(Function &function,
                                   const Candidate &candidate,
                                   const UniformSuffixPattern &pattern) {
  if (std::getenv("COMPILER_DISABLE_ORDERED_EIGHT_ROW_PANEL8_LAZY") != nullptr) {
    return false;
  }
  const auto definitions = buildDefinitions(function);
  const auto definitionBlock = definitionBlocks(function);
  const auto rowDefinition = definitions.find(pattern.row);
  const auto rowBlock = definitionBlock.find(pattern.row);
  if (rowDefinition == definitions.end() ||
      rowDefinition->second->opcode() != "phi" ||
      rowDefinition->second->operands().size() < 5 ||
      rowBlock == definitionBlock.end()) {
    return false;
  }
  auto *rowHeader = findBlock(function, rowBlock->second);
  if (rowHeader == nullptr || rowHeader->terminator() == nullptr ||
      rowHeader->terminator()->opcode() != "br" ||
      rowHeader->terminator()->operands().size() != 3) {
    return false;
  }
  const std::string fallback = rowHeader->terminator()->operands()[1];

  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  auto value = [&](const std::string &stem) {
    return freshName(valueNames, "%ordered.panel8x8.lazy." + stem);
  };
  auto block = [&](const std::string &stem) {
    return freshName(blockNames, "ordered.panel8x8.lazy." + stem);
  };

  std::array<std::string, 8> rows;
  rows[0] = pattern.row;
  for (int row = 1; row < 8; ++row) {
    rows[row] = value("row" + std::to_string(row));
  }
  std::array<std::string, 8> cutoffs;
  std::array<std::string, 8> coefficientRows;
  std::array<std::string, 8> destinationRows;
  std::array<std::string, 8> destinations;
  std::array<std::string, 8> suffixHeaders;
  std::array<std::string, 8> suffixBodies;
  std::array<std::string, 8> suffixK;
  std::array<std::string, 8> suffixAcc;
  std::array<std::string, 8> suffixHasK;
  std::array<std::string, 8> suffixAddress;
  std::array<std::string, 8> suffixCoefficient;
  std::array<std::string, 8> suffixProduct;
  std::array<std::string, 8> suffixNextAcc;
  std::array<std::string, 8> suffixNextK;
  for (int row = 0; row < 8; ++row) {
    const auto index = std::to_string(row);
    cutoffs[row] = value("cutoff" + index);
    coefficientRows[row] = value("coefficient.row" + index);
    destinationRows[row] = value("destination.row" + index);
    destinations[row] = value("destination" + index);
    suffixHeaders[row] = block("suffix" + index + ".header");
    suffixBodies[row] = block("suffix" + index + ".body");
    suffixK[row] = value("suffix" + index + ".k");
    suffixAcc[row] = value("suffix" + index + ".acc");
    suffixHasK[row] = value("suffix" + index + ".has.k");
    suffixAddress[row] = value("suffix" + index + ".address");
    suffixCoefficient[row] = value("suffix" + index + ".coefficient");
    suffixProduct[row] = value("suffix" + index + ".product");
    suffixNextAcc[row] = value("suffix" + index + ".acc.next");
    suffixNextK[row] = value("suffix" + index + ".k.next");
  }

  const auto guardName = block("guard");
  const auto setupName = block("setup");
  const auto columnHeaderName = block("column.header");
  const auto columnBodyName = block("column.body");
  const auto prefixHeaderName = block("prefix.header");
  const auto prefixBodyName = block("prefix.body");
  const auto highHeaderName = block("high.header");
  const auto highBodyName = block("high.body");
  const auto storeName = block("store");
  const auto rowLatchName = block("row.latch");
  const auto hasRow7 = value("has.row7");
  const auto columnRemainder = value("column.remainder");
  const auto fullColumns = value("full.columns");
  const auto usePanel = value("use");
  const auto threshold = value("threshold");
  std::array<std::string, 8> below;
  for (int row = 0; row < 8; ++row) {
    below[row] = value("below" + std::to_string(row));
  }
  const auto column = value("column");
  const auto hasColumn = value("has.column");
  const auto nextColumn = value("column.next");
  const auto prefixK = value("prefix.k");
  const auto prefixHasK = value("prefix.has.k");
  const auto prefixSourceRow = value("prefix.source.row");
  const auto prefixSource = value("prefix.source");
  const auto prefixNextK = value("prefix.k.next");
  const auto highStart = value("high.start");
  const auto highK = value("high.k");
  const auto highHasK = value("high.has.k");
  const auto highSourceRow = value("high.source.row");
  const auto highSource = value("high.source");
  const auto highNextK = value("high.k.next");
  std::array<std::string, 8> prefixCoefficientAddress;
  std::array<std::string, 8> prefixCoefficient;
  std::array<std::string, 8> highCoefficientAddress;
  std::array<std::string, 8> highCoefficient;
  for (int row = 0; row < 8; ++row) {
    const auto index = std::to_string(row);
    prefixCoefficientAddress[row] =
        value("prefix.coefficient" + index + ".address");
    prefixCoefficient[row] = value("prefix.coefficient" + index);
    highCoefficientAddress[row] =
        value("high.coefficient" + index + ".address");
    highCoefficient[row] = value("high.coefficient" + index);
  }
  const bool useIndexedLaneCoefficients =
      std::getenv("COMPILER_DISABLE_AARCH64_INDEXED_LANE_COEFFICIENTS") ==
      nullptr;
  const auto nextRow = value("row.next");

  std::array<std::array<std::string, 8>, 8> dependencyGuards;
  std::array<std::array<std::string, 8>, 8> dependencyBodies;
  std::array<std::array<std::string, 8>, 8> dependencyIncluded;
  std::array<std::array<std::string, 8>, 8> dependencyAddresses;
  std::array<std::array<std::string, 8>, 8> dependencyCoefficients;
  for (int destination = 0; destination < 8; ++destination) {
    for (int source = 0; source < 8; ++source) {
      const auto stem = "dep.r" + std::to_string(destination) + ".s" +
                        std::to_string(source);
      dependencyGuards[destination][source] = block(stem + ".guard");
      dependencyBodies[destination][source] = block(stem + ".body");
      dependencyIncluded[destination][source] = value(stem + ".included");
      dependencyAddresses[destination][source] = value(stem + ".address");
      dependencyCoefficients[destination][source] =
          value(stem + ".coefficient");
    }
  }

  auto rowTerminator = rowHeader->terminator()->operands();
  rowTerminator[1] = guardName;
  rowHeader->instructions().back() =
      Instruction{"br", std::move(rowTerminator)};

  BasicBlock guard{guardName};
  for (int row = 1; row < 8; ++row) {
    guard.addInstruction(Instruction{
        "add", {pattern.row, std::to_string(row)}, rows[row]});
  }
  guard.addInstruction(
      Instruction{"icmp.slt", {rows[7], candidate.outerBound}, hasRow7});
  guard.addInstruction(
      Instruction{"srem", {candidate.outerBound, "8"}, columnRemainder});
  guard.addInstruction(
      Instruction{"icmp.eq", {columnRemainder, "0"}, fullColumns});
  guard.addInstruction(Instruction{"and", {hasRow7, fullColumns}, usePanel});
  guard.addInstruction(
      Instruction{"br", {usePanel, setupName, fallback}});

  BasicBlock setup{setupName};
  setup.addInstruction(Instruction{
      "sdiv",
      {candidate.outerBound, std::to_string(pattern.thresholdDivisor)},
      threshold});
  for (int row = 0; row < 8; ++row) {
    setup.addInstruction(
        Instruction{"icmp.slt", {rows[row], threshold}, below[row]});
    setup.addInstruction(
        Instruction{"select", {below[row], threshold, rows[row]},
                    cutoffs[row]});
    setup.addInstruction(Instruction{"getelementptr",
                                     {pattern.coefficientMatrix, rows[row]},
                                     coefficientRows[row]});
    setup.addInstruction(Instruction{"getelementptr",
                                     {pattern.destinationMatrix, rows[row]},
                                     destinationRows[row]});
  }
  setup.addInstruction(Instruction{"br", {suffixHeaders[0]}});

  std::vector<BasicBlock> generated;
  for (int row = 0; row < 8; ++row) {
    const std::string incoming =
        row == 0 ? setupName : suffixHeaders[row - 1];
    const std::string next =
        row == 7 ? columnHeaderName : suffixHeaders[row + 1];
    BasicBlock header{suffixHeaders[row]};
    header.addInstruction(
        Instruction{"phi",
                    {"int", cutoffs[row], incoming, suffixNextK[row],
                     suffixBodies[row]},
                    suffixK[row]});
    header.addInstruction(
        Instruction{"phi",
                    {"int", "0", incoming, suffixNextAcc[row],
                     suffixBodies[row]},
                    suffixAcc[row]});
    header.addInstruction(Instruction{
        "icmp.slt", {suffixK[row], candidate.outerBound}, suffixHasK[row]});
    header.addInstruction(Instruction{
        "br", {suffixHasK[row], suffixBodies[row], next}});
    generated.push_back(std::move(header));

    BasicBlock body{suffixBodies[row]};
    body.addInstruction(Instruction{"getelementptr",
                                    {coefficientRows[row], suffixK[row]},
                                    suffixAddress[row]});
    body.addInstruction(Instruction{
        "load", {"int", suffixAddress[row]}, suffixCoefficient[row]});
    body.addInstruction(
        Instruction{"mul",
                    {suffixCoefficient[row],
                     std::to_string(pattern.fillValue)},
                    suffixProduct[row]});
    body.addInstruction(Instruction{
        "add", {suffixAcc[row], suffixProduct[row]}, suffixNextAcc[row]});
    body.addInstruction(
        Instruction{"add", {suffixK[row], "1"}, suffixNextK[row]});
    body.addInstruction(Instruction{"br", {suffixHeaders[row]}});
    generated.push_back(std::move(body));
  }

  BasicBlock columnHeader{columnHeaderName};
  columnHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", suffixHeaders[7], nextColumn, storeName},
                  column});
  columnHeader.addInstruction(
      Instruction{"icmp.slt", {column, candidate.outerBound}, hasColumn});
  columnHeader.addInstruction(
      Instruction{"br", {hasColumn, columnBodyName, rowLatchName}});
  generated.push_back(std::move(columnHeader));

  BasicBlock columnBody{columnBodyName};
  for (int row = 0; row < 8; ++row) {
    columnBody.addInstruction(Instruction{
        "getelementptr", {destinationRows[row], column}, destinations[row]});
  }
  for (int row = 0; row < 8; ++row) {
    columnBody.addInstruction(Instruction{
        "aarch64.neon.ordered.panel8.i32x8.lazy.begin.row" +
            std::to_string(row),
        {suffixAcc[row]}});
  }
  columnBody.addInstruction(
      Instruction{"add", {pattern.row, "8"}, highStart});
  columnBody.addInstruction(Instruction{"br", {prefixHeaderName}});
  generated.push_back(std::move(columnBody));

  BasicBlock prefixHeader{prefixHeaderName};
  prefixHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", columnBodyName, prefixNextK, prefixBodyName},
                  prefixK});
  prefixHeader.addInstruction(
      Instruction{"icmp.slt", {prefixK, pattern.row}, prefixHasK});
  prefixHeader.addInstruction(
      Instruction{"br", {prefixHasK, prefixBodyName, highHeaderName}});
  generated.push_back(std::move(prefixHeader));

  BasicBlock prefixBody{prefixBodyName};
  prefixBody.addInstruction(Instruction{
      "getelementptr", {pattern.destinationMatrix, prefixK}, prefixSourceRow});
  prefixBody.addInstruction(Instruction{
      "getelementptr", {prefixSourceRow, column}, prefixSource});
  for (int row = 0; row < 8; ++row) {
    if (useIndexedLaneCoefficients) {
      prefixBody.addInstruction(Instruction{
          "aarch64.neon.ordered.panel8.i32x8.lazy.coefficient.row" +
              std::to_string(row),
          {coefficientRows[row], prefixK}});
    } else {
      prefixBody.addInstruction(Instruction{
          "getelementptr", {coefficientRows[row], prefixK},
          prefixCoefficientAddress[row]});
      prefixBody.addInstruction(Instruction{
          "load", {"int", prefixCoefficientAddress[row]},
          prefixCoefficient[row]});
    }
  }
  prefixBody.addInstruction(Instruction{
      "aarch64.neon.ordered.panel8.i32x8.lazy.shared.load", {prefixSource}});
  for (int row = 0; row < 8; ++row) {
    prefixBody.addInstruction(Instruction{
        "aarch64.neon.ordered.panel8.i32x8.lazy.shared.row" +
            std::to_string(row),
        useIndexedLaneCoefficients
            ? std::vector<std::string>{}
            : std::vector<std::string>{prefixCoefficient[row]}});
  }
  prefixBody.addInstruction(
      Instruction{"add", {prefixK, "1"}, prefixNextK});
  prefixBody.addInstruction(Instruction{"br", {prefixHeaderName}});
  generated.push_back(std::move(prefixBody));

  BasicBlock highHeader{highHeaderName};
  highHeader.addInstruction(
      Instruction{"phi",
                  {"int", highStart, prefixHeaderName, highNextK, highBodyName},
                  highK});
  highHeader.addInstruction(
      Instruction{"icmp.slt", {highK, cutoffs[0]}, highHasK});
  highHeader.addInstruction(Instruction{
      "br", {highHasK, highBodyName, dependencyGuards[0][0]}});
  generated.push_back(std::move(highHeader));

  BasicBlock highBody{highBodyName};
  highBody.addInstruction(Instruction{
      "getelementptr", {pattern.destinationMatrix, highK}, highSourceRow});
  highBody.addInstruction(
      Instruction{"getelementptr", {highSourceRow, column}, highSource});
  for (int row = 0; row < 8; ++row) {
    if (useIndexedLaneCoefficients) {
      highBody.addInstruction(Instruction{
          "aarch64.neon.ordered.panel8.i32x8.lazy.coefficient.row" +
              std::to_string(row),
          {coefficientRows[row], highK}});
    } else {
      highBody.addInstruction(Instruction{
          "getelementptr", {coefficientRows[row], highK},
          highCoefficientAddress[row]});
      highBody.addInstruction(Instruction{
          "load", {"int", highCoefficientAddress[row]},
          highCoefficient[row]});
    }
  }
  highBody.addInstruction(Instruction{
      "aarch64.neon.ordered.panel8.i32x8.lazy.shared.load", {highSource}});
  for (int row = 0; row < 8; ++row) {
    highBody.addInstruction(Instruction{
        "aarch64.neon.ordered.panel8.i32x8.lazy.shared.row" +
            std::to_string(row),
        useIndexedLaneCoefficients
            ? std::vector<std::string>{}
            : std::vector<std::string>{highCoefficient[row]}});
  }
  highBody.addInstruction(Instruction{"add", {highK, "1"}, highNextK});
  highBody.addInstruction(Instruction{"br", {highHeaderName}});
  generated.push_back(std::move(highBody));

  for (int destination = 0; destination < 8; ++destination) {
    for (int source = 0; source < 8; ++source) {
      const int linear = destination * 8 + source;
      const std::string next =
          linear == 63
              ? storeName
              : dependencyGuards[(linear + 1) / 8][(linear + 1) % 8];
      BasicBlock dependencyGuard{
          dependencyGuards[destination][source]};
      dependencyGuard.addInstruction(
          Instruction{"icmp.slt", {rows[source], cutoffs[destination]},
                      dependencyIncluded[destination][source]});
      dependencyGuard.addInstruction(Instruction{
          "br", {dependencyIncluded[destination][source],
                 dependencyBodies[destination][source], next}});
      generated.push_back(std::move(dependencyGuard));

      BasicBlock dependencyBody{dependencyBodies[destination][source]};
      dependencyBody.addInstruction(Instruction{
          "getelementptr", {coefficientRows[destination], rows[source]},
          dependencyAddresses[destination][source]});
      dependencyBody.addInstruction(Instruction{
          "load", {"int", dependencyAddresses[destination][source]},
          dependencyCoefficients[destination][source]});
      const bool useNewValue = source < destination;
      const std::string version = useNewValue ? "new" : "old";
      std::vector<std::string> operands;
      if (!useNewValue) {
        operands.push_back(destinations[source]);
      }
      operands.push_back(dependencyCoefficients[destination][source]);
      dependencyBody.addInstruction(Instruction{
          "aarch64.neon.ordered.panel8.i32x8.lazy." + version +
              std::to_string(source) + ".to.row" +
              std::to_string(destination),
          std::move(operands)});
      dependencyBody.addInstruction(Instruction{"br", {next}});
      generated.push_back(std::move(dependencyBody));
    }
  }

  BasicBlock store{storeName};
  for (int row = 0; row < 8; ++row) {
    store.addInstruction(Instruction{
        "aarch64.neon.ordered.panel8.i32x8.lazy.store.row" +
            std::to_string(row),
        {destinations[row]}});
  }
  store.addInstruction(Instruction{"add", {column, "8"}, nextColumn});
  store.addInstruction(Instruction{"br", {columnHeaderName}});
  generated.push_back(std::move(store));

  BasicBlock rowLatch{rowLatchName};
  rowLatch.addInstruction(
      Instruction{"add", {pattern.row, "8"}, nextRow});
  rowLatch.addInstruction(Instruction{"br", {rowBlock->second}});
  generated.push_back(std::move(rowLatch));

  auto rowPhiOperands = rowDefinition->second->operands();
  rowPhiOperands.push_back(nextRow);
  rowPhiOperands.push_back(rowLatchName);
  for (auto &instruction : rowHeader->instructions()) {
    if (instruction.result() == pattern.row && instruction.opcode() == "phi") {
      instruction =
          Instruction{"phi", std::move(rowPhiOperands), pattern.row};
      break;
    }
  }

  function.addBlock(std::move(guard));
  function.addBlock(std::move(setup));
  for (auto &generatedBlock : generated) {
    function.addBlock(std::move(generatedBlock));
  }
  return true;
}

std::vector<BasicBlock> cloneTailBlocks(
    const Function& function, const Candidate& candidate,
    std::unordered_set<std::string>& valueNames,
    std::unordered_set<std::string>& blockNames, std::string& tailHeader) {
  const std::vector<std::string> originals{
      candidate.outerHeader, candidate.outerBody, candidate.innerHeader,
      candidate.innerBody, candidate.innerExit};
  const auto cfg = buildCFG(function);
  std::unordered_map<std::string, std::string> blockMap;
  for (const auto& name : originals) {
    blockMap.emplace(name, freshName(blockNames, "unrolljam.tail"));
  }
  tailHeader = blockMap.at(candidate.outerHeader);

  std::unordered_map<std::string, std::string> valueMap;
  for (const auto& name : originals) {
    const auto* block = findBlock(function, cfg, name);
    if (block == nullptr) {
      continue;
    }
    for (const auto& instruction : block->instructions()) {
      if (instruction.hasResult()) {
        valueMap.emplace(instruction.result(),
                         freshName(valueNames, "%unrolljam.tail"));
      }
    }
  }

  std::vector<BasicBlock> clones;
  for (const auto& name : originals) {
    const auto* original = findBlock(function, cfg, name);
    BasicBlock clone{blockMap.at(name)};
    for (const auto& instruction : original->instructions()) {
      auto operands = instruction.operands();
      for (auto& operand : operands) {
        if (const auto value = valueMap.find(operand);
            value != valueMap.end()) {
          operand = value->second;
        }
      }
      if (instruction.opcode() == "br") {
        const std::size_t firstTarget = operands.size() == 1 ? 0 : 1;
        for (std::size_t index = firstTarget; index < operands.size();
             ++index) {
          if (const auto target = blockMap.find(operands[index]);
              target != blockMap.end()) {
            operands[index] = target->second;
          }
        }
      }
      const std::string result = instruction.hasResult()
                                     ? valueMap.at(instruction.result())
                                     : std::string{};
      clone.addInstruction(
          Instruction{instruction.opcode(), std::move(operands), result});
    }
    clones.push_back(std::move(clone));
  }
  return clones;
}

std::vector<Instruction> cloneLane(
    const std::vector<Instruction>& original, int lane,
    const Candidate& candidate, const std::string& laneValue,
    const std::string& laneReductionSlot, bool keepInnerUpdate,
    std::unordered_set<std::string>& valueNames,
    std::unordered_map<std::string, std::string>& laneZeroAddresses) {
  std::unordered_set<std::string> directMemoryAddresses;
  std::unordered_set<std::string> gepBases;
  for (const auto& instruction : original) {
    if (instruction.opcode() == "load" && instruction.operands().size() == 2) {
      directMemoryAddresses.insert(instruction.operands()[1]);
    } else if (instruction.opcode() == "store" &&
               instruction.operands().size() == 3) {
      directMemoryAddresses.insert(instruction.operands()[2]);
    }
    if (isGepOpcode(instruction.opcode()) &&
        instruction.operands().size() == 2) {
      gepBases.insert(instruction.operands()[0]);
    }
  }

  std::unordered_map<std::string, std::string> replacements;
  std::vector<Instruction> cloned;
  for (const auto& instruction : original) {
    if (instruction.isTerminator()) {
      continue;
    }
    if (isLoadFrom(instruction, candidate.outerSlot)) {
      replacements[instruction.result()] = laneValue;
      continue;
    }
    if (isStoreTo(instruction, candidate.outerSlot) ||
        (!keepInnerUpdate && isStoreTo(instruction, candidate.innerSlot))) {
      continue;
    }

    auto operands = instruction.operands();
    for (auto& operand : operands) {
      if (operand == candidate.reductionSlot) {
        operand = laneReductionSlot;
      }
      if (const auto replacement = replacements.find(operand);
          replacement != replacements.end()) {
        operand = replacement->second;
      }
    }
    std::string result;
    if (instruction.hasResult()) {
      result = freshName(valueNames, "%unrolljam.lane" + std::to_string(lane));
      replacements[instruction.result()] = result;
    }
    std::string opcode = instruction.opcode();
    // A non-terminal GEP can select an entire array row, whose stride is not a
    // scalar lane offset. Only share addresses consumed directly by memory and
    // never used as the base of another GEP.
    const bool isTerminalScalarAddress =
        instruction.hasResult() &&
        directMemoryAddresses.find(instruction.result()) !=
            directMemoryAddresses.end() &&
        gepBases.find(instruction.result()) == gepBases.end();
    if (isGepOpcode(opcode) && isTerminalScalarAddress &&
        operands.size() == 2 && operands[1] == laneValue) {
      if (lane == 0) {
        laneZeroAddresses.emplace(instruction.result(), result);
      } else if (const auto anchor =
                     laneZeroAddresses.find(instruction.result());
                 anchor != laneZeroAddresses.end()) {
        opcode = "gep.linear";
        operands = {anchor->second, std::to_string(lane)};
      }
    }
    cloned.emplace_back(std::move(opcode), std::move(operands),
                        std::move(result));
  }
  return cloned;
}

bool collapseJammedAddReductionToNeon(
    Function& function, BasicBlock& outerBody, BasicBlock& innerBody,
    BasicBlock& innerExit, const std::vector<std::string>& reductionSlots) {
  const auto fail = [](const char* reason) {
    (void)reason;
    return false;
  };
  if (reductionSlots.size() != 32) return fail("lane count");

  // Fixed vector registers are an implicit state of the three pseudos below.
  // Conservatively reject functions that already contain floating-point or
  // target-vector operations so their live ranges can never overlap it.
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if ((!instruction.opcode().empty() && instruction.opcode()[0] == 'f') ||
          instruction.opcode().rfind("aarch64.neon.", 0) == 0 ||
          std::find(instruction.operands().begin(), instruction.operands().end(),
                    "float") != instruction.operands().end()) {
        return fail("preexisting vector or float");
      }
    }
  }

  const auto analyzeDefinitions = [](const std::vector<Instruction>& code) {
    std::unordered_map<std::string, std::size_t> definitions;
    for (std::size_t index = 0; index < code.size(); ++index) {
      if (code[index].hasResult()) definitions.emplace(code[index].result(), index);
    }
    return definitions;
  };
  const auto bodyDefinitions = analyzeDefinitions(innerBody.instructions());

  struct LaneRecurrence {
    std::size_t reductionLoad = 0;
    std::size_t sourceAddress = 0;
    std::size_t sourceLoad = 0;
    std::size_t multiply = 0;
    std::size_t add = 0;
    std::size_t reductionStore = 0;
    std::string coefficient;
    std::string sourceAddressValue;
  };
  std::vector<LaneRecurrence> lanes(32);

  for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
    std::optional<std::size_t> loadIndex;
    std::optional<std::size_t> storeIndex;
    for (std::size_t index = 0; index < innerBody.instructions().size(); ++index) {
      const auto& instruction = innerBody.instructions()[index];
      if (isLoadFrom(instruction, reductionSlots[lane])) {
        if (loadIndex.has_value()) return false;
        loadIndex = index;
      }
      if (isStoreTo(instruction, reductionSlots[lane])) {
        if (storeIndex.has_value()) return false;
        storeIndex = index;
      }
    }
    if (!loadIndex.has_value() || !storeIndex.has_value()) return fail("body reduction access");
    const auto& store = innerBody.instructions()[*storeIndex];
    const auto addFound = bodyDefinitions.find(store.operands()[1]);
    if (addFound == bodyDefinitions.end()) return fail("body add definition");
    const auto& add = innerBody.instructions()[addFound->second];
    if (add.opcode() != "add" || add.operands().size() != 2) return fail("body add shape");
    const auto& reductionValue = innerBody.instructions()[*loadIndex].result();
    std::string productValue;
    if (add.operands()[0] == reductionValue) productValue = add.operands()[1];
    if (add.operands()[1] == reductionValue) productValue = add.operands()[0];
    const auto multiplyFound = bodyDefinitions.find(productValue);
    if (multiplyFound == bodyDefinitions.end()) return fail("body multiply definition");
    const auto& multiply = innerBody.instructions()[multiplyFound->second];
    if (multiply.opcode() != "mul" || multiply.operands().size() != 2) return fail("body multiply shape");

    lanes[lane].reductionLoad = *loadIndex;
    lanes[lane].multiply = multiplyFound->second;
    lanes[lane].add = addFound->second;
    lanes[lane].reductionStore = *storeIndex;
  }

  // Lane one establishes which multiply operand is the consecutive source;
  // the other must be one common loop-invariant/broadcast coefficient.
  bool foundSource = false;
  std::string sourceBase;
  std::string coefficient;
  const auto& laneOneMultiply = innerBody.instructions()[lanes[1].multiply];
  for (std::size_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
    const auto loadFound = bodyDefinitions.find(laneOneMultiply.operands()[operandIndex]);
    if (loadFound == bodyDefinitions.end()) continue;
    const auto& load = innerBody.instructions()[loadFound->second];
    if (load.opcode() != "load" || load.operands().size() != 2) continue;
    const auto addressFound = bodyDefinitions.find(load.operands()[1]);
    if (addressFound == bodyDefinitions.end()) continue;
    const auto& address = innerBody.instructions()[addressFound->second];
    long long offset = 0;
    if (address.opcode() != "gep.linear" || address.operands().size() != 2 ||
        !detail::parseIntegerLiteral(address.operands()[1], offset) || offset != 1) {
      continue;
    }
    sourceBase = address.operands()[0];
    foundSource = true;
    break;
  }
  if (!foundSource) return fail("lane one source");

  std::function<bool(const std::string&, const std::string&,
                     std::unordered_set<std::string>&)>
      equivalentValue;
  equivalentValue = [&](const std::string& lhs, const std::string& rhs,
                        std::unordered_set<std::string>& visiting) {
    if (lhs == rhs) return true;
    if (!visiting.insert(lhs + "\n" + rhs).second) return false;
    const auto lhsFound = bodyDefinitions.find(lhs);
    const auto rhsFound = bodyDefinitions.find(rhs);
    if (lhsFound == bodyDefinitions.end() || rhsFound == bodyDefinitions.end()) {
      return false;
    }
    const auto& lhsInstruction = innerBody.instructions()[lhsFound->second];
    const auto& rhsInstruction = innerBody.instructions()[rhsFound->second];
    if (lhsInstruction.opcode() != rhsInstruction.opcode() ||
        lhsInstruction.operands().size() != rhsInstruction.operands().size()) {
      return false;
    }
    for (std::size_t index = 0; index < lhsInstruction.operands().size(); ++index) {
      if (!equivalentValue(lhsInstruction.operands()[index],
                           rhsInstruction.operands()[index], visiting)) {
        return false;
      }
    }
    return true;
  };

  std::unordered_set<std::size_t> bodyRemovals;
  std::size_t bodyInsert = innerBody.instructions().size();
  std::string coefficientAddress;
  for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
    const auto& multiply = innerBody.instructions()[lanes[lane].multiply];
    std::optional<std::size_t> sourceLoad;
    std::optional<std::size_t> sourceAddress;
    std::string laneCoefficient;
    for (std::size_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
      const auto loadFound = bodyDefinitions.find(multiply.operands()[operandIndex]);
      if (loadFound == bodyDefinitions.end()) continue;
      const auto& load = innerBody.instructions()[loadFound->second];
      if (load.opcode() != "load" || load.operands().size() != 2) continue;
      if (lane == 0 && load.operands()[1] == sourceBase) {
        sourceLoad = loadFound->second;
        laneCoefficient = multiply.operands()[1 - operandIndex];
      } else if (lane != 0) {
        const auto addressFound = bodyDefinitions.find(load.operands()[1]);
        if (addressFound == bodyDefinitions.end()) continue;
        const auto& address = innerBody.instructions()[addressFound->second];
        long long offset = 0;
        if (address.opcode() == "gep.linear" && address.operands().size() == 2 &&
            address.operands()[0] == sourceBase &&
            detail::parseIntegerLiteral(address.operands()[1], offset) &&
            offset == static_cast<long long>(lane)) {
          sourceLoad = loadFound->second;
          sourceAddress = addressFound->second;
          laneCoefficient = multiply.operands()[1 - operandIndex];
        }
      }
    }
    if (!sourceLoad.has_value() || (lane != 0 && !sourceAddress.has_value())) {
      return fail("lane source consistency");
    }
    const auto coefficientFound = bodyDefinitions.find(laneCoefficient);
    if (coefficientFound == bodyDefinitions.end()) {
      return fail("coefficient definition");
    }
    const auto& coefficientLoad =
        innerBody.instructions()[coefficientFound->second];
    if (coefficientLoad.opcode() != "load" ||
        coefficientLoad.operands().size() != 2) {
      return fail("coefficient load");
    }
    if (lane == 0) {
      coefficient = laneCoefficient;
      coefficientAddress = coefficientLoad.operands()[1];
    } else {
      std::unordered_set<std::string> visiting;
      if (!equivalentValue(coefficientAddress,
                           coefficientLoad.operands()[1], visiting)) {
        return fail("coefficient address consistency");
      }
    }
    lanes[lane].sourceLoad = *sourceLoad;
    lanes[lane].sourceAddress = sourceAddress.value_or(0);
    bodyInsert = std::min(bodyInsert, *sourceLoad);
    bodyRemovals.insert(lanes[lane].reductionLoad);
    bodyRemovals.insert(*sourceLoad);
    if (sourceAddress.has_value()) bodyRemovals.insert(*sourceAddress);
    bodyRemovals.insert(lanes[lane].multiply);
    bodyRemovals.insert(lanes[lane].add);
    bodyRemovals.insert(lanes[lane].reductionStore);
  }

  const auto exitDefinitions = analyzeDefinitions(innerExit.instructions());
  std::unordered_set<std::size_t> exitRemovals;
  std::string destinationBase;
  std::size_t exitInsert = innerExit.instructions().size();
  for (std::size_t lane = 0; lane < reductionSlots.size(); ++lane) {
    std::optional<std::size_t> loadIndex;
    std::optional<std::size_t> storeIndex;
    for (std::size_t index = 0; index < innerExit.instructions().size(); ++index) {
      const auto& instruction = innerExit.instructions()[index];
      if (isLoadFrom(instruction, reductionSlots[lane])) loadIndex = index;
      if (instruction.opcode() == "store" && instruction.operands().size() == 3 &&
          loadIndex.has_value() &&
          instruction.operands()[1] == innerExit.instructions()[*loadIndex].result()) {
        storeIndex = index;
      }
    }
    if (!loadIndex.has_value() || !storeIndex.has_value()) return fail("exit load/store");
    const std::string addressValue = innerExit.instructions()[*storeIndex].operands()[2];
    const auto addressFound = exitDefinitions.find(addressValue);
    if (addressFound == exitDefinitions.end()) return fail("exit address definition");
    const auto& address = innerExit.instructions()[addressFound->second];
    if (lane == 0) {
      destinationBase = addressValue;
      exitInsert = *storeIndex;
    } else {
      long long offset = 0;
      if (address.opcode() != "gep.linear" || address.operands().size() != 2 ||
          address.operands()[0] != destinationBase ||
          !detail::parseIntegerLiteral(address.operands()[1], offset) ||
          offset != static_cast<long long>(lane)) {
        return fail("exit address consistency");
      }
      exitRemovals.insert(addressFound->second);
    }
    exitRemovals.insert(*loadIndex);
    exitRemovals.insert(*storeIndex);
  }

  std::vector<Instruction> vectorBody;
  for (std::size_t index = 0; index < innerBody.instructions().size(); ++index) {
    if (index == bodyInsert) {
      vectorBody.emplace_back("aarch64.neon.dup.acc.i32x32",
                              std::vector<std::string>{coefficient});
      vectorBody.emplace_back("aarch64.neon.mla.acc.i32x32",
                              std::vector<std::string>{sourceBase});
    }
    if (bodyRemovals.find(index) == bodyRemovals.end()) {
      vectorBody.push_back(std::move(innerBody.instructions()[index]));
    }
  }
  innerBody.instructions() = std::move(vectorBody);

  std::vector<Instruction> vectorExit;
  for (std::size_t index = 0; index < innerExit.instructions().size(); ++index) {
    if (index == exitInsert) {
      vectorExit.emplace_back("aarch64.neon.store.acc.i32x32",
                              std::vector<std::string>{destinationBase});
    }
    if (exitRemovals.find(index) == exitRemovals.end()) {
      vectorExit.push_back(std::move(innerExit.instructions()[index]));
    }
  }
  innerExit.instructions() = std::move(vectorExit);

  auto& outerInstructions = outerBody.instructions();
  std::string initialValue;
  for (const auto& instruction : outerInstructions) {
    if (!isStoreTo(instruction, reductionSlots[0])) continue;
    if (!initialValue.empty()) return fail("multiple accumulator initializers");
    initialValue = instruction.operands()[1];
  }
  if (initialValue.empty()) return fail("missing accumulator initializer");
  std::vector<Instruction> vectorOuter;
  for (auto& instruction : outerInstructions) {
    if (isStoreTo(instruction, reductionSlots[0]) ||
        std::any_of(reductionSlots.begin() + 1, reductionSlots.end(),
                    [&](const auto& slot) {
                      return isStoreTo(instruction, slot) ||
                             (instruction.opcode() == "alloca" &&
                              instruction.result() == slot);
                    })) {
      continue;
    }
    if (instruction.isTerminator()) {
      if (isIntegerValue(initialValue, 0)) {
        vectorOuter.emplace_back("aarch64.neon.zero.acc.i32x32",
                                 std::vector<std::string>{});
      } else {
        vectorOuter.emplace_back("aarch64.neon.splat.acc.i32x32",
                                 std::vector<std::string>{initialValue});
      }
    }
    vectorOuter.push_back(std::move(instruction));
  }
  outerInstructions = std::move(vectorOuter);

  for (auto& block : function.blocks()) {
    auto& instructions = block.instructions();
    instructions.erase(
        std::remove_if(instructions.begin(), instructions.end(),
                       [&](const Instruction& instruction) {
                         return instruction.opcode() == "alloca" &&
                                std::find(reductionSlots.begin(),
                                          reductionSlots.end(),
                                          instruction.result()) !=
                                    reductionSlots.end();
                       }),
        instructions.end());
  }
  return true;
}

bool transform(Function& function, const Candidate& candidate, int jamFactor,
               bool useAArch64VectorAccumulator) {
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  std::string tailHeader;
  auto tailBlocks =
      cloneTailBlocks(function, candidate, valueNames, blockNames, tailHeader);

  auto* outerHeader = findBlock(function, candidate.outerHeader);
  auto* outerBody = findBlock(function, candidate.outerBody);
  auto* innerBody = findBlock(function, candidate.innerBody);
  auto* innerExit = findBlock(function, candidate.innerExit);
  if (outerHeader == nullptr || outerBody == nullptr || innerBody == nullptr ||
      innerExit == nullptr) {
    return false;
  }

  const auto originalInnerBody = innerBody->instructions();
  const auto originalInnerExit = innerExit->instructions();
  std::vector<std::string> laneValues{candidate.outerValue};
  std::vector<std::string> laneReductionSlots{candidate.reductionSlot};

  auto& outerBodyInstructions = outerBody->instructions();
  auto insert = outerBodyInstructions.end() - 1;
  for (int lane = 1; lane < jamFactor; ++lane) {
    const auto laneSlot = freshName(valueNames, "%unrolljam.reduction");
    laneReductionSlots.push_back(laneSlot);
    insert = outerBodyInstructions.insert(
        insert, Instruction{"alloca", {candidate.reductionType}, laneSlot});
    ++insert;
    insert = outerBodyInstructions.insert(
        insert, Instruction{"store",
                            {candidate.reductionType,
                             candidate.reductionIdentity, laneSlot}});
    ++insert;
    const auto laneValue = freshName(valueNames, "%unrolljam.index");
    laneValues.push_back(laneValue);
    insert = outerBodyInstructions.insert(
        insert,
        Instruction{
            "add", {candidate.outerValue, std::to_string(lane)}, laneValue});
    ++insert;
  }

  std::vector<Instruction> jammedBody;
  std::unordered_map<std::string, std::string> bodyLaneZeroAddresses;
  for (int lane = 0; lane < jamFactor; ++lane) {
    auto cloned =
        cloneLane(originalInnerBody, lane, candidate, laneValues[lane],
                  laneReductionSlots[lane], lane == jamFactor - 1, valueNames,
                  bodyLaneZeroAddresses);
    jammedBody.insert(jammedBody.end(), cloned.begin(), cloned.end());
  }
  jammedBody.emplace_back("br",
                          std::vector<std::string>{candidate.innerHeader});
  innerBody->instructions() = std::move(jammedBody);

  std::vector<Instruction> jammedExit;
  std::unordered_map<std::string, std::string> exitLaneZeroAddresses;
  for (int lane = 0; lane < jamFactor; ++lane) {
    auto cloned = cloneLane(originalInnerExit, lane, candidate,
                            laneValues[lane], laneReductionSlots[lane], true,
                            valueNames, exitLaneZeroAddresses);
    jammedExit.insert(jammedExit.end(), cloned.begin(), cloned.end());
  }
  const auto nextOuter = freshName(valueNames, "%unrolljam.next");
  jammedExit.emplace_back("add",
                          std::vector<std::string>{candidate.outerValue,
                                                   std::to_string(jamFactor)},
                          nextOuter);
  jammedExit.emplace_back(
      "store", std::vector<std::string>{candidate.reductionType, nextOuter,
                                        candidate.outerSlot});
  jammedExit.emplace_back("br",
                          std::vector<std::string>{candidate.outerHeader});
  innerExit->instructions() = std::move(jammedExit);

  if (useAArch64VectorAccumulator && jamFactor == 32) {
    collapseJammedAddReductionToNeon(function, *outerBody, *innerBody,
                                     *innerExit, laneReductionSlots);
  }

  const auto guardName = freshName(blockNames, "unrolljam.guard");
  const auto remaining = freshName(valueNames, "%unrolljam.remaining");
  const auto enough = freshName(valueNames, "%unrolljam.enough");
  BasicBlock guard{guardName};
  guard.addInstruction(Instruction{
      "sub", {candidate.outerBound, candidate.outerValue}, remaining});
  guard.addInstruction(Instruction{
      "icmp.sgt", {remaining, std::to_string(jamFactor - 1)}, enough});
  guard.addInstruction(
      Instruction{"br", {enough, candidate.outerBody, tailHeader}});

  auto headerTerminator = outerHeader->instructions().back().operands();
  headerTerminator[1] = guardName;
  outerHeader->instructions().back() =
      Instruction{"br", std::move(headerTerminator)};

  function.addBlock(std::move(guard));
  for (auto& block : tailBlocks) {
    function.addBlock(std::move(block));
  }
  return true;
}

}  // namespace

void LoopUnrollAndJamPass::run(compiler::ir::core::Module& module) {
  if (jamFactor_ < 2) return;
  for (auto& function : module.functions()) {
    auto candidate = findCandidate(function);
    if (candidate.has_value()) {
      std::optional<UniformSuffixPattern> uniformPattern;
      if (useAArch64VectorAccumulator_) {
        uniformPattern = findUniformSuffixPattern(function, *candidate);
        foldUniformSuffixContribution(function, *candidate);
      }
      const bool transformed =
          transform(function, *candidate, jamFactor_,
                    useAArch64VectorAccumulator_);
      if (transformed && uniformPattern.has_value() &&
          useAArch64VectorAccumulator_) {
        if (buildOrderedTwoRowPanel(function, *candidate, *uniformPattern)) {
          buildOrderedEightRowPanel8Lazy(function, *candidate, *uniformPattern);
        }
      }
    }
  }
}

}  // namespace compiler::midend::pass
