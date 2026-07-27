#include "../../../include/compiler/midend/pass/LoopUnrollAndJamPass.h"

#include <algorithm>
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

struct PanelInterchangeInfo {
  std::string enclosingHeader;
  std::string enclosingPreheader;
  std::string enclosingBody;
  std::string enclosingLatch;
  std::string enclosingIndex;
  std::string coefficientBase;
  std::string destinationBase;
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
  std::optional<PanelInterchangeInfo> panelInterchange;
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

std::optional<PanelInterchangeInfo> findPanelInterchangeInfo(
    const Function& function, const CFG& cfg,
    const DefinitionMap& definitions,
    const std::unordered_set<std::string>& scalarSlots,
    const std::string& outerPreheader, const std::string& outerHeader,
    const std::string& outerExit, const std::string& outerSlot,
    const std::string& innerSlot, const std::string& bound,
    const BasicBlock& innerBody, const std::string& destination) {
  const auto preheaderIndex = cfg.index.find(outerPreheader);
  if (preheaderIndex == cfg.index.end() ||
      cfg.predecessors[preheaderIndex->second].size() != 1) {
    return std::nullopt;
  }
  const std::string enclosingHeader =
      cfg.predecessors[preheaderIndex->second].front();
  const auto* header = findBlock(function, cfg, enclosingHeader);
  const auto* body = findBlock(function, cfg, outerPreheader);
  const auto* latch = findBlock(function, cfg, outerExit);
  if (header == nullptr || body == nullptr || latch == nullptr ||
      header->instructions().size() != 3 ||
      header->instructions()[0].opcode() != "phi" ||
      header->instructions()[0].operands().size() != 5 ||
      header->instructions()[1].opcode() != "icmp.slt" ||
      header->instructions()[1].operands().size() != 2 ||
      header->instructions()[2].opcode() != "br" ||
      header->instructions()[2].operands().size() != 3 ||
      header->instructions()[2].operands()[0] !=
          header->instructions()[1].result() ||
      header->instructions()[2].operands()[1] != outerPreheader ||
      header->instructions()[1].operands()[0] !=
          header->instructions()[0].result() ||
      header->instructions()[1].operands()[1] != bound ||
      body->terminator() == nullptr || body->terminator()->opcode() != "br" ||
      body->terminator()->operands() != std::vector<std::string>{outerHeader} ||
      latch->terminator() == nullptr ||
      latch->terminator()->opcode() != "br" ||
      latch->terminator()->operands() !=
          std::vector<std::string>{enclosingHeader}) {
    return std::nullopt;
  }

  const auto& rowPhi = header->instructions()[0];
  const std::string row = rowPhi.result();
  std::string enclosingPreheader;
  std::string rowNext;
  for (std::size_t index = 1; index + 1 < rowPhi.operands().size();
       index += 2) {
    if (rowPhi.operands()[index] == "0") {
      enclosingPreheader = rowPhi.operands()[index + 1];
    } else if (rowPhi.operands()[index + 1] == outerExit) {
      rowNext = rowPhi.operands()[index];
    }
  }
  const auto next = definitions.find(rowNext);
  if (enclosingPreheader.empty() || next == definitions.end() ||
      next->second->opcode() != "add" ||
      next->second->operands().size() != 2 ||
      !((next->second->operands()[0] == row &&
         next->second->operands()[1] == "1") ||
        (next->second->operands()[1] == row &&
         next->second->operands()[0] == "1"))) {
    return std::nullopt;
  }

  const auto destinationPath = pointerPath(destination, definitions);
  if (!destinationPath.has_value() ||
      destinationPath->indices.size() != 2 ||
      destinationPath->indices[0] != row ||
      !isValueLoadedFromSlot(destinationPath->indices[1], outerSlot,
                             definitions)) {
    return std::nullopt;
  }

  bool foundSource = false;
  std::optional<std::string> coefficientBase;
  for (const auto& instruction : innerBody.instructions()) {
    if (instruction.opcode() != "load" ||
        instruction.operands().size() != 2 ||
        scalarSlots.find(instruction.operands()[1]) != scalarSlots.end()) {
      continue;
    }
    const auto path = pointerPath(instruction.operands()[1], definitions);
    if (!path.has_value() || path->indices.size() != 2) {
      return std::nullopt;
    }
    if (path->base == destinationPath->base &&
        isValueLoadedFromSlot(path->indices[0], innerSlot, definitions) &&
        isValueLoadedFromSlot(path->indices[1], outerSlot, definitions)) {
      foundSource = true;
      continue;
    }
    if (path->base != destinationPath->base && path->indices[0] == row &&
        isValueLoadedFromSlot(path->indices[1], innerSlot, definitions) &&
        (!coefficientBase.has_value() || *coefficientBase == path->base)) {
      coefficientBase = path->base;
      continue;
    }
    return std::nullopt;
  }
  if (!foundSource || !coefficientBase.has_value()) {
    return std::nullopt;
  }

  return PanelInterchangeInfo{
      enclosingHeader, enclosingPreheader, outerPreheader, outerExit, row,
      *coefficientBase, destinationPath->base};
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
    if (!innerCondition.has_value() || innerTerminator == nullptr ||
        innerCondition->bound != outerCondition->bound) {
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

    const auto panelInterchange = findPanelInterchangeInfo(
        function, cfg, definitions, scalarSlots, outerPreheader,
        outerHeader.name(), outerExitName, outerCondition->slot,
        innerCondition->slot, outerCondition->bound, *innerBody,
        *destination);

    return Candidate{
        outerHeader.name(),    outerBodyName,         outerExitName,
        innerHeaderName,       innerBodyName,         innerExitName,
        outerCondition->slot,  innerCondition->slot,  reductions[0].first,
        outerCondition->value, outerCondition->bound, *reductionType,
        reductions[0].second,  panelInterchange};
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

bool interchangeVectorPanelWithEnclosingLoop(
    Function& function, const Candidate& candidate,
    std::unordered_set<std::string>& valueNames,
    std::unordered_set<std::string>& blockNames) {
  if (!candidate.panelInterchange.has_value()) return false;
  const auto& panel = *candidate.panelInterchange;
  auto* enclosingHeader = findBlock(function, panel.enclosingHeader);
  auto* enclosingPreheader = findBlock(function, panel.enclosingPreheader);
  auto* enclosingBody = findBlock(function, panel.enclosingBody);
  if (enclosingHeader == nullptr || enclosingPreheader == nullptr ||
      enclosingBody == nullptr || enclosingHeader->instructions().empty() ||
      enclosingHeader->instructions().front().opcode() != "phi" ||
      enclosingPreheader->terminator() == nullptr ||
      enclosingPreheader->terminator()->opcode() != "br") {
    return false;
  }

  const std::string prepareName =
      freshName(blockNames, "panel.interchange.prepare");
  const std::string panelHeaderName =
      freshName(blockNames, "panel.interchange.panel.header");
  const std::string rowHeaderName =
      freshName(blockNames, "panel.interchange.row.header");
  const std::string rowBodyName =
      freshName(blockNames, "panel.interchange.row.body");
  const std::string kHeaderName =
      freshName(blockNames, "panel.interchange.k.header");
  const std::string kBodyName =
      freshName(blockNames, "panel.interchange.k.body");
  const std::string storeName =
      freshName(blockNames, "panel.interchange.store");
  const std::string panelLatchName =
      freshName(blockNames, "panel.interchange.panel.latch");

  const std::string vectorEnd =
      freshName(valueNames, "%panel.interchange.vector.end");
  const std::string column =
      freshName(valueNames, "%panel.interchange.column");
  const std::string hasPanel =
      freshName(valueNames, "%panel.interchange.has.panel");
  const std::string row =
      freshName(valueNames, "%panel.interchange.row");
  const std::string hasRow =
      freshName(valueNames, "%panel.interchange.has.row");
  const std::string coefficientRow =
      freshName(valueNames, "%panel.interchange.coefficient.row");
  const std::string destinationRow =
      freshName(valueNames, "%panel.interchange.destination.row");
  const std::string k = freshName(valueNames, "%panel.interchange.k");
  const std::string hasK =
      freshName(valueNames, "%panel.interchange.has.k");
  const std::string coefficientAddress =
      freshName(valueNames, "%panel.interchange.coefficient.address");
  const std::string coefficient =
      freshName(valueNames, "%panel.interchange.coefficient");
  const std::string sourceRow =
      freshName(valueNames, "%panel.interchange.source.row");
  const std::string source =
      freshName(valueNames, "%panel.interchange.source");
  const std::string kNext =
      freshName(valueNames, "%panel.interchange.k.next");
  const std::string destination =
      freshName(valueNames, "%panel.interchange.destination");
  const std::string rowNext =
      freshName(valueNames, "%panel.interchange.row.next");
  const std::string columnNext =
      freshName(valueNames, "%panel.interchange.column.next");

  auto preheaderBranch = enclosingPreheader->terminator()->operands();
  bool retargeted = false;
  for (auto& operand : preheaderBranch) {
    if (operand == panel.enclosingHeader) {
      operand = prepareName;
      retargeted = true;
    }
  }
  if (!retargeted) return false;

  auto phiOperands =
      enclosingHeader->instructions().front().operands();
  bool rewrotePhi = false;
  for (std::size_t index = 2; index < phiOperands.size(); index += 2) {
    if (phiOperands[index] == panel.enclosingPreheader) {
      phiOperands[index] = panelHeaderName;
      rewrotePhi = true;
    }
  }
  if (!rewrotePhi) return false;

  bool rewroteScalarStart = false;
  for (auto& instruction : enclosingBody->instructions()) {
    if (isStoreTo(instruction, candidate.outerSlot) &&
        instruction.operands()[1] == "0") {
      instruction = Instruction{
          "store", {candidate.reductionType, vectorEnd, candidate.outerSlot}};
      rewroteScalarStart = true;
    }
  }
  if (!rewroteScalarStart) return false;

  enclosingPreheader->instructions().back() =
      Instruction{"br", std::move(preheaderBranch)};
  enclosingHeader->instructions().front() =
      Instruction{"phi", std::move(phiOperands), panel.enclosingIndex};

  BasicBlock prepare{prepareName};
  prepare.addInstruction(
      Instruction{"and", {candidate.outerBound, "-32"}, vectorEnd});
  prepare.addInstruction(Instruction{"br", {panelHeaderName}});

  BasicBlock panelHeader{panelHeaderName};
  panelHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", prepareName, columnNext, panelLatchName},
                  column});
  panelHeader.addInstruction(
      Instruction{"icmp.slt", {column, vectorEnd}, hasPanel});
  panelHeader.addInstruction(
      Instruction{"br", {hasPanel, rowHeaderName, panel.enclosingHeader}});

  BasicBlock rowHeader{rowHeaderName};
  rowHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", panelHeaderName, rowNext, storeName},
                  row});
  rowHeader.addInstruction(
      Instruction{"icmp.slt", {row, candidate.outerBound}, hasRow});
  rowHeader.addInstruction(
      Instruction{"br", {hasRow, rowBodyName, panelLatchName}});

  BasicBlock rowBody{rowBodyName};
  rowBody.addInstruction(Instruction{
      "getelementptr", {panel.coefficientBase, row}, coefficientRow});
  rowBody.addInstruction(Instruction{
      "getelementptr", {panel.destinationBase, row}, destinationRow});
  rowBody.addInstruction(
      Instruction{"aarch64.neon.zero.acc.i32x32", {}});
  rowBody.addInstruction(Instruction{"br", {kHeaderName}});

  BasicBlock kHeader{kHeaderName};
  kHeader.addInstruction(
      Instruction{"phi", {"int", "0", rowBodyName, kNext, kBodyName}, k});
  kHeader.addInstruction(
      Instruction{"icmp.slt", {k, candidate.outerBound}, hasK});
  kHeader.addInstruction(Instruction{"br", {hasK, kBodyName, storeName}});

  BasicBlock kBody{kBodyName};
  kBody.addInstruction(
      Instruction{"getelementptr", {coefficientRow, k}, coefficientAddress});
  kBody.addInstruction(
      Instruction{"load", {"int", coefficientAddress}, coefficient});
  kBody.addInstruction(
      Instruction{"getelementptr", {panel.destinationBase, k}, sourceRow});
  kBody.addInstruction(
      Instruction{"getelementptr", {sourceRow, column}, source});
  kBody.addInstruction(
      Instruction{"aarch64.neon.dup.acc.i32x32", {coefficient}});
  kBody.addInstruction(
      Instruction{"aarch64.neon.mla.acc.i32x32", {source}});
  kBody.addInstruction(Instruction{"add", {k, "1"}, kNext});
  kBody.addInstruction(Instruction{"br", {kHeaderName}});

  BasicBlock store{storeName};
  store.addInstruction(
      Instruction{"getelementptr", {destinationRow, column}, destination});
  store.addInstruction(
      Instruction{"aarch64.neon.store.acc.i32x32", {destination}});
  store.addInstruction(Instruction{"add", {row, "1"}, rowNext});
  store.addInstruction(Instruction{"br", {rowHeaderName}});

  BasicBlock panelLatch{panelLatchName};
  panelLatch.addInstruction(
      Instruction{"add", {column, "32"}, columnNext});
  panelLatch.addInstruction(Instruction{"br", {panelHeaderName}});

  function.addBlock(std::move(prepare));
  function.addBlock(std::move(panelHeader));
  function.addBlock(std::move(rowHeader));
  function.addBlock(std::move(rowBody));
  function.addBlock(std::move(kHeader));
  function.addBlock(std::move(kBody));
  function.addBlock(std::move(store));
  function.addBlock(std::move(panelLatch));
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

  bool collapsedToVectorAccumulator = false;
  if (useAArch64VectorAccumulator && jamFactor == 32) {
    collapsedToVectorAccumulator = collapseJammedAddReductionToNeon(
        function, *outerBody, *innerBody, *innerExit, laneReductionSlots);
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
  if (collapsedToVectorAccumulator &&
      std::getenv("COMPILER_DISABLE_COLUMN_PANEL_INTERCHANGE") == nullptr) {
    interchangeVectorPanelWithEnclosingLoop(function, candidate, valueNames,
                                            blockNames);
  }
  return true;
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

}  // namespace compiler::midend::pass
