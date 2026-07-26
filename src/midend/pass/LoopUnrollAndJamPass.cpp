#include "../../../include/compiler/midend/pass/LoopUnrollAndJamPass.h"

#include <algorithm>
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
        reductions[0].second};
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
      vectorOuter.emplace_back("aarch64.neon.zero.acc.i32x32",
                               std::vector<std::string>{});
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

struct InterleavedSubReduction {
  std::string header;
  std::string body;
  std::string preheader;
  std::string inductionSlot;
  std::string reductionSlot;
  std::string bound;
};

std::optional<InterleavedSubReduction> findInterleavedSubReduction(
    const Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto scalarSlots = collectScalarSlots(function);

  for (const auto& header : function.blocks()) {
    const auto condition = loopCondition(header, definitions);
    const auto* terminator = header.terminator();
    if (!condition.has_value() || terminator == nullptr ||
        terminator->opcode() != "br" ||
        terminator->operands().size() != 3 ||
        (!header.instructions().empty() &&
         header.instructions().front().opcode() == "phi")) {
      continue;
    }
    const std::string bodyName = terminator->operands()[1];
    const auto* body = findBlock(function, cfg, bodyName);
    if (body == nullptr || body->terminator() == nullptr ||
        body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        hasUnsupportedControlOrCall(*body)) {
      continue;
    }

    const auto headerIndex = cfg.index.at(header.name());
    const auto bodyIndex = cfg.index.at(bodyName);
    if (cfg.predecessors[headerIndex].size() != 2 ||
        cfg.predecessors[bodyIndex] !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    std::string preheaderName;
    for (const auto& predecessor : cfg.predecessors[headerIndex]) {
      if (predecessor != bodyName) preheaderName = predecessor;
    }
    const auto* preheader = findBlock(function, cfg, preheaderName);
    if (preheader == nullptr || preheader->terminator() == nullptr ||
        preheader->terminator()->opcode() != "br" ||
        preheader->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        !hasConstantStore(*preheader, condition->slot, "0") ||
        !hasUnitUpdate(*body, condition->slot, definitions)) {
      continue;
    }
    const auto inductionType = allocaType(condition->slot, definitions);
    if (!inductionType.has_value() || *inductionType != "int") continue;

    std::optional<std::string> reductionSlot;
    for (const auto& instruction : preheader->instructions()) {
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3 ||
          instruction.operands()[2] == condition->slot ||
          scalarSlots.find(instruction.operands()[2]) == scalarSlots.end()) {
        continue;
      }
      if (reductionSlot.has_value()) {
        reductionSlot.reset();
        break;
      }
      reductionSlot = instruction.operands()[2];
    }
    if (!reductionSlot.has_value()) continue;
    const auto reductionType = allocaType(*reductionSlot, definitions);
    if (!reductionType.has_value() || *reductionType != "int") continue;

    const auto reductionLoad = reductionLoadResult(*body, *reductionSlot);
    const Instruction* reductionStore = nullptr;
    std::size_t inductionStores = 0;
    std::size_t nonscalarLoads = 0;
    std::unordered_set<std::string> nonscalarLoadValues;
    bool supported = true;
    for (const auto& instruction : body->instructions()) {
      const bool supportedOpcode =
          instruction.opcode() == "load" ||
          instruction.opcode() == "store" ||
          isGepOpcode(instruction.opcode()) ||
          instruction.opcode() == "mul" ||
          instruction.opcode() == "sub" ||
          instruction.opcode() == "add" ||
          instruction.opcode() == "br";
      if (!supportedOpcode) {
        supported = false;
        break;
      }
      if (isStoreTo(instruction, condition->slot)) ++inductionStores;
      if (isStoreTo(instruction, *reductionSlot)) {
        if (reductionStore != nullptr) {
          supported = false;
          break;
        }
        reductionStore = &instruction;
      }
      if (instruction.opcode() == "store" &&
          !isStoreTo(instruction, condition->slot) &&
          !isStoreTo(instruction, *reductionSlot)) {
        supported = false;
        break;
      }
      if (instruction.opcode() == "load" &&
          instruction.operands().size() == 2 &&
          scalarSlots.find(instruction.operands()[1]) ==
              scalarSlots.end()) {
        ++nonscalarLoads;
        nonscalarLoadValues.insert(instruction.result());
      }
    }
    if (!supported || !reductionLoad.has_value() ||
        reductionStore == nullptr || inductionStores != 1 ||
        nonscalarLoads != 2) {
      continue;
    }

    const auto subtractFound =
        definitions.find(reductionStore->operands()[1]);
    if (subtractFound == definitions.end() ||
        subtractFound->second->opcode() != "sub" ||
        subtractFound->second->operands().size() != 2 ||
        subtractFound->second->operands()[0] != *reductionLoad) {
      continue;
    }
    const auto multiplyFound =
        definitions.find(subtractFound->second->operands()[1]);
    if (multiplyFound == definitions.end() ||
        multiplyFound->second->opcode() != "mul" ||
        multiplyFound->second->operands().size() != 2 ||
        nonscalarLoadValues.find(
            multiplyFound->second->operands()[0]) ==
            nonscalarLoadValues.end() ||
        nonscalarLoadValues.find(
            multiplyFound->second->operands()[1]) ==
            nonscalarLoadValues.end()) {
      continue;
    }

    return InterleavedSubReduction{
        header.name(), bodyName, preheaderName, condition->slot,
        *reductionSlot, condition->bound};
  }
  return std::nullopt;
}

std::vector<Instruction> cloneInterleavedLane(
    const BasicBlock& body, const InterleavedSubReduction& candidate,
    const std::string& inductionValue, const std::string& reductionSlot,
    std::unordered_set<std::string>& valueNames) {
  std::unordered_map<std::string, std::string> replacements;
  std::vector<Instruction> cloned;
  for (const auto& instruction : body.instructions()) {
    if (instruction.isTerminator()) continue;
    if (isLoadFrom(instruction, candidate.inductionSlot)) {
      replacements[instruction.result()] = inductionValue;
      continue;
    }
    if (isStoreTo(instruction, candidate.inductionSlot)) continue;

    auto operands = instruction.operands();
    for (auto& operand : operands) {
      if (operand == candidate.reductionSlot) operand = reductionSlot;
      if (const auto found = replacements.find(operand);
          found != replacements.end()) {
        operand = found->second;
      }
    }
    std::string result;
    if (instruction.hasResult()) {
      result = freshName(valueNames, "%interleave.lane");
      replacements[instruction.result()] = result;
    }
    cloned.emplace_back(instruction.opcode(), std::move(operands),
                        std::move(result));
  }
  return cloned;
}

bool interleaveOneScalarSubReduction(
    Function& function, const InterleavedSubReduction& candidate) {
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  auto* preheader = findBlock(function, candidate.preheader);
  const auto* originalBody = findBlock(function, candidate.body);
  if (preheader == nullptr || originalBody == nullptr ||
      preheader->terminator() == nullptr) {
    return false;
  }

  const std::string pairHeaderName =
      freshName(blockNames, "interleave.pair.header");
  const std::string pairBodyName =
      freshName(blockNames, "interleave.pair.body");
  const std::string combineName =
      freshName(blockNames, "interleave.combine");
  const std::string secondarySlot =
      freshName(valueNames, "%interleave.reduction");
  const std::string induction =
      freshName(valueNames, "%interleave.index");
  const std::string nextInduction =
      freshName(valueNames, "%interleave.index.next");
  const std::string hasPair =
      freshName(valueNames, "%interleave.has.pair");
  const std::string afterPair =
      freshName(valueNames, "%interleave.index.after.pair");
  const std::string primaryValue =
      freshName(valueNames, "%interleave.primary");
  const std::string secondaryValue =
      freshName(valueNames, "%interleave.secondary");
  const std::string combinedValue =
      freshName(valueNames, "%interleave.combined");

  auto preheaderBranch = preheader->terminator()->operands();
  if (preheaderBranch != std::vector<std::string>{candidate.header}) {
    return false;
  }
  auto insert = preheader->instructions().end() - 1;
  insert = preheader->instructions().insert(
      insert, Instruction{"alloca", {"int"}, secondarySlot});
  ++insert;
  preheader->instructions().insert(
      insert, Instruction{"store", {"int", "0", secondarySlot}});
  preheader->instructions().back() =
      Instruction{"br", {pairHeaderName}};

  BasicBlock pairHeader{pairHeaderName};
  pairHeader.addInstruction(
      Instruction{"load", {"int", candidate.inductionSlot}, induction});
  pairHeader.addInstruction(
      Instruction{"add", {induction, "1"}, nextInduction});
  pairHeader.addInstruction(
      Instruction{"icmp.slt", {nextInduction, candidate.bound}, hasPair});
  pairHeader.addInstruction(
      Instruction{"br", {hasPair, pairBodyName, combineName}});

  BasicBlock pairBody{pairBodyName};
  auto firstLane =
      cloneInterleavedLane(*originalBody, candidate, induction,
                           candidate.reductionSlot, valueNames);
  auto secondLane =
      cloneInterleavedLane(*originalBody, candidate, nextInduction,
                           secondarySlot, valueNames);
  for (auto& instruction : firstLane) {
    pairBody.addInstruction(std::move(instruction));
  }
  for (auto& instruction : secondLane) {
    pairBody.addInstruction(std::move(instruction));
  }
  pairBody.addInstruction(
      Instruction{"add", {induction, "2"}, afterPair});
  pairBody.addInstruction(Instruction{
      "store", {"int", afterPair, candidate.inductionSlot}});
  pairBody.addInstruction(Instruction{"br", {pairHeaderName}});

  BasicBlock combine{combineName};
  combine.addInstruction(
      Instruction{"load", {"int", candidate.reductionSlot}, primaryValue});
  combine.addInstruction(
      Instruction{"load", {"int", secondarySlot}, secondaryValue});
  combine.addInstruction(
      Instruction{"add", {primaryValue, secondaryValue}, combinedValue});
  combine.addInstruction(Instruction{
      "store", {"int", combinedValue, candidate.reductionSlot}});
  combine.addInstruction(Instruction{"br", {candidate.header}});

  auto header =
      std::find_if(function.blocks().begin(), function.blocks().end(),
                   [&](const BasicBlock& block) {
                     return block.name() == candidate.header;
                   });
  if (header == function.blocks().end()) return false;
  header = function.blocks().insert(header, std::move(pairHeader));
  ++header;
  header = function.blocks().insert(header, std::move(pairBody));
  ++header;
  function.blocks().insert(header, std::move(combine));
  return true;
}

struct SSAInterleavedSubReduction {
  std::string header;
  std::string body;
  std::string preheader;
  std::string induction;
  std::string reduction;
  std::string inductionBackedge;
  std::string reductionBackedge;
  std::string initialReduction;
  std::string bound;
};

std::optional<std::pair<std::string, std::string>> phiIncoming(
    const Instruction& phi, const std::string& predecessor) {
  if (phi.opcode() != "phi") return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) {
      return std::pair<std::string, std::string>{
          phi.operands()[index], predecessor};
    }
  }
  return std::nullopt;
}

std::optional<SSAInterleavedSubReduction>
findSSAInterleavedSubReduction(const Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  const auto scalarSlots = collectScalarSlots(function);

  for (const auto& header : function.blocks()) {
    const auto* terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    std::vector<const Instruction*> phis;
    for (const auto& instruction : header.instructions()) {
      if (instruction.opcode() == "phi") phis.push_back(&instruction);
    }
    if (phis.size() != 2 || phis[0]->operands().size() != 5 ||
        phis[1]->operands().size() != 5) {
      continue;
    }
    const auto conditionFound =
        definitions.find(terminator->operands()[0]);
    if (conditionFound == definitions.end() ||
        conditionFound->second->opcode() != "icmp.slt" ||
        conditionFound->second->operands().size() != 2) {
      continue;
    }
    const std::string induction =
        conditionFound->second->operands()[0];
    const Instruction* inductionPhi = nullptr;
    const Instruction* reductionPhi = nullptr;
    for (const auto* phi : phis) {
      if (phi->result() == induction) {
        inductionPhi = phi;
      } else {
        reductionPhi = phi;
      }
    }
    if (inductionPhi == nullptr || reductionPhi == nullptr) continue;

    const std::string bodyName = terminator->operands()[1];
    const auto* body = findBlock(function, cfg, bodyName);
    if (body == nullptr || body->terminator() == nullptr ||
        body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        hasUnsupportedControlOrCall(*body)) {
      continue;
    }
    const auto headerIndex = cfg.index.at(header.name());
    const auto bodyIndex = cfg.index.at(bodyName);
    if (cfg.predecessors[headerIndex].size() != 2 ||
        cfg.predecessors[bodyIndex] !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    std::string preheaderName;
    for (const auto& predecessor : cfg.predecessors[headerIndex]) {
      if (predecessor != bodyName) preheaderName = predecessor;
    }
    const auto* preheader = findBlock(function, cfg, preheaderName);
    if (preheader == nullptr || preheader->terminator() == nullptr ||
        preheader->terminator()->opcode() != "br" ||
        preheader->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const auto inductionInitial =
        phiIncoming(*inductionPhi, preheaderName);
    const auto inductionBackedge =
        phiIncoming(*inductionPhi, bodyName);
    const auto reductionInitial =
        phiIncoming(*reductionPhi, preheaderName);
    const auto reductionBackedge =
        phiIncoming(*reductionPhi, bodyName);
    if (!inductionInitial.has_value() ||
        inductionInitial->first != "0" ||
        !inductionBackedge.has_value() ||
        !reductionInitial.has_value() ||
        !reductionBackedge.has_value()) {
      continue;
    }

    const auto inductionUpdate =
        definitions.find(inductionBackedge->first);
    if (inductionUpdate == definitions.end() ||
        inductionUpdate->second->opcode() != "add" ||
        inductionUpdate->second->operands().size() != 2 ||
        !((inductionUpdate->second->operands()[0] == induction &&
           inductionUpdate->second->operands()[1] == "1") ||
          (inductionUpdate->second->operands()[1] == induction &&
           inductionUpdate->second->operands()[0] == "1"))) {
      continue;
    }

    std::size_t nonscalarLoads = 0;
    std::unordered_set<std::string> nonscalarLoadValues;
    bool supported = true;
    for (const auto& instruction : body->instructions()) {
      const bool supportedOpcode =
          instruction.opcode() == "load" ||
          isGepOpcode(instruction.opcode()) ||
          instruction.opcode() == "mul" ||
          instruction.opcode() == "sub" ||
          instruction.opcode() == "add" ||
          instruction.opcode() == "br";
      if (!supportedOpcode || instruction.opcode() == "store") {
        supported = false;
        break;
      }
      if (instruction.opcode() == "load" &&
          instruction.operands().size() == 2 &&
          scalarSlots.find(instruction.operands()[1]) ==
              scalarSlots.end()) {
        ++nonscalarLoads;
        nonscalarLoadValues.insert(instruction.result());
      }
    }
    if (!supported || nonscalarLoads != 2) continue;

    const auto subtractFound =
        definitions.find(reductionBackedge->first);
    if (subtractFound == definitions.end() ||
        subtractFound->second->opcode() != "sub" ||
        subtractFound->second->operands().size() != 2 ||
        subtractFound->second->operands()[0] !=
            reductionPhi->result()) {
      continue;
    }
    const auto multiplyFound =
        definitions.find(subtractFound->second->operands()[1]);
    if (multiplyFound == definitions.end() ||
        multiplyFound->second->opcode() != "mul" ||
        multiplyFound->second->operands().size() != 2 ||
        nonscalarLoadValues.find(
            multiplyFound->second->operands()[0]) ==
            nonscalarLoadValues.end() ||
        nonscalarLoadValues.find(
            multiplyFound->second->operands()[1]) ==
            nonscalarLoadValues.end()) {
      continue;
    }

    return SSAInterleavedSubReduction{
        header.name(),
        bodyName,
        preheaderName,
        induction,
        reductionPhi->result(),
        inductionBackedge->first,
        reductionBackedge->first,
        reductionInitial->first,
        conditionFound->second->operands()[1]};
  }
  return std::nullopt;
}

struct SSAInterleavedLane {
  std::vector<Instruction> instructions;
  std::string inductionBackedge;
  std::string reductionBackedge;
};

SSAInterleavedLane cloneSSAInterleavedLane(
    const BasicBlock& body, const SSAInterleavedSubReduction& candidate,
    const std::string& induction, const std::string& reduction,
    std::unordered_set<std::string>& valueNames) {
  std::unordered_map<std::string, std::string> replacements{
      {candidate.induction, induction},
      {candidate.reduction, reduction}};
  SSAInterleavedLane lane;
  for (const auto& instruction : body.instructions()) {
    if (instruction.isTerminator()) continue;
    auto operands = instruction.operands();
    for (auto& operand : operands) {
      if (const auto found = replacements.find(operand);
          found != replacements.end()) {
        operand = found->second;
      }
    }
    std::string result;
    if (instruction.hasResult()) {
      result = freshName(valueNames, "%ssa.interleave.lane");
      replacements[instruction.result()] = result;
    }
    lane.instructions.emplace_back(instruction.opcode(),
                                   std::move(operands), std::move(result));
  }
  lane.inductionBackedge =
      replacements.at(candidate.inductionBackedge);
  lane.reductionBackedge =
      replacements.at(candidate.reductionBackedge);
  return lane;
}

bool interleaveOneSSASubReduction(
    Function& function, const SSAInterleavedSubReduction& candidate) {
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  auto* preheader = findBlock(function, candidate.preheader);
  auto* headerBlock = findBlock(function, candidate.header);
  const auto* body = findBlock(function, candidate.body);
  if (preheader == nullptr || headerBlock == nullptr || body == nullptr ||
      preheader->terminator() == nullptr) {
    return false;
  }

  const std::string pairHeaderName =
      freshName(blockNames, "ssa.interleave.header");
  const std::string pairBodyName =
      freshName(blockNames, "ssa.interleave.body");
  const std::string combineName =
      freshName(blockNames, "ssa.interleave.combine");
  const std::string pairIndex =
      freshName(valueNames, "%ssa.interleave.index");
  const std::string primary =
      freshName(valueNames, "%ssa.interleave.primary");
  const std::string secondary =
      freshName(valueNames, "%ssa.interleave.secondary");
  const std::string nextIndex =
      freshName(valueNames, "%ssa.interleave.next");
  const std::string hasPair =
      freshName(valueNames, "%ssa.interleave.has.pair");
  const std::string combined =
      freshName(valueNames, "%ssa.interleave.combined");

  auto firstLane =
      cloneSSAInterleavedLane(*body, candidate, pairIndex, primary,
                              valueNames);
  auto secondLane =
      cloneSSAInterleavedLane(*body, candidate, nextIndex, secondary,
                              valueNames);

  bool changedInductionPhi = false;
  bool changedReductionPhi = false;
  for (auto& instruction : headerBlock->instructions()) {
    if (instruction.opcode() != "phi") continue;
    auto operands = instruction.operands();
    for (std::size_t index = 1; index + 1 < operands.size(); index += 2) {
      if (operands[index + 1] != candidate.preheader) continue;
      if (instruction.result() == candidate.induction) {
        operands[index] = pairIndex;
        operands[index + 1] = combineName;
        changedInductionPhi = true;
      } else if (instruction.result() == candidate.reduction) {
        operands[index] = combined;
        operands[index + 1] = combineName;
        changedReductionPhi = true;
      }
    }
    instruction =
        Instruction{"phi", std::move(operands), instruction.result()};
  }
  if (!changedInductionPhi || !changedReductionPhi) return false;
  preheader->instructions().back() =
      Instruction{"br", {pairHeaderName}};

  BasicBlock pairHeader{pairHeaderName};
  pairHeader.addInstruction(Instruction{
      "phi",
      {"int", "0", candidate.preheader,
       secondLane.inductionBackedge, pairBodyName},
      pairIndex});
  pairHeader.addInstruction(Instruction{
      "phi",
      {"int", candidate.initialReduction, candidate.preheader,
       firstLane.reductionBackedge, pairBodyName},
      primary});
  pairHeader.addInstruction(Instruction{
      "phi",
      {"int", "0", candidate.preheader,
       secondLane.reductionBackedge, pairBodyName},
      secondary});
  pairHeader.addInstruction(
      Instruction{"add", {pairIndex, "1"}, nextIndex});
  pairHeader.addInstruction(
      Instruction{"icmp.slt", {nextIndex, candidate.bound}, hasPair});
  pairHeader.addInstruction(
      Instruction{"br", {hasPair, pairBodyName, combineName}});

  BasicBlock pairBody{pairBodyName};
  for (auto& instruction : firstLane.instructions) {
    pairBody.addInstruction(std::move(instruction));
  }
  for (auto& instruction : secondLane.instructions) {
    pairBody.addInstruction(std::move(instruction));
  }
  pairBody.addInstruction(Instruction{"br", {pairHeaderName}});

  BasicBlock combineBlock{combineName};
  combineBlock.addInstruction(
      Instruction{"add", {primary, secondary}, combined});
  combineBlock.addInstruction(Instruction{"br", {candidate.header}});

  auto header =
      std::find_if(function.blocks().begin(), function.blocks().end(),
                   [&](const BasicBlock& block) {
                     return block.name() == candidate.header;
                   });
  if (header == function.blocks().end()) return false;
  header = function.blocks().insert(header, std::move(pairHeader));
  ++header;
  header = function.blocks().insert(header, std::move(pairBody));
  ++header;
  function.blocks().insert(header, std::move(combineBlock));
  return true;
}

void interleaveScalarSubReductions(Function& function) {
  constexpr std::size_t kMaximumReductions = 8;
  for (std::size_t count = 0; count < kMaximumReductions; ++count) {
    const auto candidate = findSSAInterleavedSubReduction(function);
    if (!candidate.has_value() ||
        !interleaveOneSSASubReduction(function, *candidate)) {
      break;
    }
  }
  for (std::size_t count = 0; count < kMaximumReductions; ++count) {
    const auto candidate = findInterleavedSubReduction(function);
    if (!candidate.has_value() ||
        !interleaveOneScalarSubReduction(function, *candidate)) {
      break;
    }
  }
}

}  // namespace

void LoopUnrollAndJamPass::run(compiler::ir::core::Module& module) {
  if (jamFactor_ < 2) return;
  for (auto& function : module.functions()) {
    const auto candidate = findCandidate(function);
    if (candidate.has_value()) {
      transform(function, *candidate, jamFactor_,
                useAArch64VectorAccumulator_);
    }
  }
}

void ScalarSubReductionInterleavingPass::run(
    compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_SCALAR_SUB_REDUCTION_INTERLEAVING") !=
      nullptr) {
    return;
  }
  for (auto& function : module.functions()) {
    interleaveScalarSubReductions(function);
  }
}

}  // namespace compiler::midend::pass
