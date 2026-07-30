#include "../../../include/compiler/midend/pass/TriangularTransposeShadowPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/core/Constant.h"
#include "../../../include/compiler/ir/core/GlobalVariable.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Constant;
using compiler::ir::core::Function;
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

struct Definition {
  const Instruction *instruction = nullptr;
  std::string block;
};
using Definitions = std::unordered_map<std::string, Definition>;

struct AccessPath {
  std::string root;
  std::vector<std::string> indices;
};

struct Candidate {
  std::size_t functionIndex = 0;
  std::size_t bound = 0;
  std::string matrix;
  std::string outer;
  std::string middle;
  std::string innerNext;
  std::string current;
  std::string targetAddress;
  std::string rowBase;
  std::string outerBody;
  std::string innerBody;
  std::string columnRowGep;
  std::string columnElementGep;
  std::string guardPreheader;
  std::string guard;
  std::string init;
  std::string commit;
  std::string middleLatch;
};

bool isGep(const Instruction &instruction) {
  const auto &opcode = instruction.opcode();
  return opcode == "getelementptr" || opcode == "getelementptr.linear" ||
         opcode == "gep" || opcode == "gep.linear";
}

std::unordered_map<std::string, const BasicBlock *>
blockMap(const Function &function) {
  std::unordered_map<std::string, const BasicBlock *> result;
  for (const auto &block : function.blocks())
    result.emplace(block.name(), &block);
  return result;
}

Definitions definitionsOf(const Function &function) {
  Definitions result;
  for (const auto &block : function.blocks())
    for (const auto &instruction : block.instructions())
      if (instruction.hasResult())
        result.emplace(instruction.result(),
                       Definition{&instruction, block.name()});
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
predecessorsOf(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> result;
  for (const auto &block : function.blocks())
    result.emplace(block.name(), std::vector<std::string>{});
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index)
      if (auto found = result.find(operands[index]); found != result.end())
        found->second.push_back(block.name());
  }
  return result;
}

std::optional<AccessPath> accessPath(const std::string &address,
                                     const Definitions &definitions) {
  AccessPath path{address, {}};
  std::unordered_set<std::string> visited;
  while (!path.root.empty() && path.root.front() == '%' &&
         visited.insert(path.root).second) {
    const auto found = definitions.find(path.root);
    if (found == definitions.end() || !isGep(*found->second.instruction))
      break;
    const auto &operands = found->second.instruction->operands();
    if (operands.size() != 2)
      return std::nullopt;
    path.indices.insert(path.indices.begin(), operands[1]);
    path.root = operands[0];
  }
  return path.indices.empty() ? std::nullopt : std::optional{path};
}

std::optional<std::pair<std::string, std::string>>
phiIncoming(const Instruction &phi, const std::string &predecessor) {
  if (phi.opcode() != "phi")
    return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2)
    if (phi.operands()[index + 1] == predecessor)
      return std::pair{phi.operands()[index], phi.operands()[index + 1]};
  return std::nullopt;
}

std::optional<std::pair<std::string, std::string>>
otherPhiIncoming(const Instruction &phi, const std::string &predecessor) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5)
    return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2)
    if (phi.operands()[index + 1] != predecessor)
      return std::pair{phi.operands()[index], phi.operands()[index + 1]};
  return std::nullopt;
}

bool isAddConstant(const Definitions &definitions, const std::string &value,
                   const std::string &base, long long constant) {
  const auto found = definitions.find(value);
  if (found == definitions.end())
    return false;
  const auto &instruction = *found->second.instruction;
  if (instruction.opcode() != "add" || instruction.operands().size() != 2)
    return false;
  long long literal = 0;
  return (instruction.operands()[0] == base &&
          detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
          literal == constant) ||
         (instruction.operands()[1] == base &&
          detail::parseIntegerLiteral(instruction.operands()[0], literal) &&
          literal == constant);
}

bool isSubOne(const Definitions &definitions, const std::string &value,
              const std::string &base) {
  const auto found = definitions.find(value);
  return found != definitions.end() &&
         found->second.instruction->opcode() == "sub" &&
         found->second.instruction->operands() ==
             std::vector<std::string>{base, "1"};
}

std::optional<std::size_t> matrixExtent(const Module &module,
                                        const Function &function,
                                        const std::string &matrix) {
  static const std::regex parameterType{R"(^int\[\]\[([1-9][0-9]*)\]$)"};
  static const std::regex globalType{
      R"(^int\[([1-9][0-9]*)\]\[([1-9][0-9]*)\]$)"};
  std::smatch match;
  for (const auto &parameter : function.parameters()) {
    if (parameter.name != matrix ||
        !std::regex_match(parameter.type, match, parameterType))
      continue;
    try {
      return static_cast<std::size_t>(std::stoull(match[1].str()));
    } catch (...) {
      return std::nullopt;
    }
  }
  if (matrix.size() > 1 && matrix.front() == '@') {
    for (const auto &global : module.globals()) {
      if ("@" + global.name() != matrix ||
          !std::regex_match(global.type(), match, globalType) ||
          match[1].str() != match[2].str())
        continue;
      try {
        return static_cast<std::size_t>(std::stoull(match[1].str()));
      } catch (...) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

bool reaches(const std::unordered_map<std::string, const BasicBlock *> &blocks,
             const std::string &start, const std::string &target,
             const std::unordered_set<std::string> &stops) {
  std::vector<std::string> work{start};
  std::unordered_set<std::string> seen;
  while (!work.empty()) {
    const auto current = std::move(work.back());
    work.pop_back();
    if (current == target)
      return true;
    if (!seen.insert(current).second || stops.find(current) != stops.end())
      continue;
    const auto found = blocks.find(current);
    if (found == blocks.end() || found->second->terminator() == nullptr)
      continue;
    const auto &operands = found->second->terminator()->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index)
      work.push_back(operands[index]);
  }
  return false;
}

std::optional<std::unordered_set<std::string>>
outerRegion(const std::unordered_map<std::string, const BasicBlock *> &blocks,
            const std::string &body, const std::string &header,
            const std::string &exit) {
  std::vector<std::string> work{body};
  std::unordered_set<std::string> seen;
  while (!work.empty()) {
    const auto current = std::move(work.back());
    work.pop_back();
    if (current == header)
      continue;
    if (current == exit)
      return std::nullopt;
    if (!seen.insert(current).second)
      continue;
    const auto found = blocks.find(current);
    if (found == blocks.end() || found->second->terminator() == nullptr)
      return std::nullopt;
    const auto &operands = found->second->terminator()->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index)
      if (operands[index] != header)
        work.push_back(operands[index]);
  }
  return seen;
}

std::optional<std::string> transparentDestination(
    const std::unordered_map<std::string, const BasicBlock *> &blocks,
    std::string blockName) {
  std::unordered_set<std::string> seen;
  bool first = true;
  while (seen.insert(blockName).second) {
    const auto found = blocks.find(blockName);
    if (found == blocks.end() || found->second->terminator() == nullptr ||
        found->second->terminator()->opcode() != "br" ||
        found->second->terminator()->operands().size() != 1)
      return first ? std::nullopt : std::optional{blockName};
    const auto &instructions = found->second->instructions();
    if (!first && instructions.size() != 1)
      return blockName;
    first = false;
    blockName = found->second->terminator()->operands()[0];
  }
  return std::nullopt;
}

bool onlyUse(const Function &function, const std::string &value,
             const std::string &userResult) {
  std::size_t uses = 0;
  for (const auto &block : function.blocks())
    for (const auto &instruction : block.instructions())
      for (const auto &operand : instruction.operands())
        if (operand == value) {
          ++uses;
          if (instruction.result() != userResult)
            return false;
        }
  return uses == 1;
}

std::optional<Candidate> matchFunction(const Module &module,
                                       std::size_t functionIndex) {
  const auto &function = module.functions()[functionIndex];
  const auto blocks = blockMap(function);
  const auto definitions = definitionsOf(function);
  const auto predecessors = predecessorsOf(function);

  for (const auto &header : function.blocks()) {
    const auto *terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3)
      continue;
    const auto condition = definitions.find(terminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second.block != header.name() ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2)
      continue;
    const std::string inner = condition->second.instruction->operands()[0];
    const std::string middle = condition->second.instruction->operands()[1];
    const auto innerPhiDef = definitions.find(inner);
    if (innerPhiDef == definitions.end() ||
        innerPhiDef->second.block != header.name() ||
        innerPhiDef->second.instruction->opcode() != "phi" ||
        innerPhiDef->second.instruction->operands().size() != 5)
      continue;

    const std::string innerBodyName = terminator->operands()[1];
    const std::string commitName = terminator->operands()[2];
    const auto bodyFound = blocks.find(innerBodyName);
    const auto commitFound = blocks.find(commitName);
    const auto bodyDestination = transparentDestination(blocks, innerBodyName);
    if (bodyFound == blocks.end() || commitFound == blocks.end() ||
        bodyFound->second->terminator() == nullptr ||
        !bodyDestination.has_value() || *bodyDestination != header.name() ||
        commitFound->second->instructions().size() != 2 ||
        commitFound->second->instructions()[0].opcode() != "store" ||
        commitFound->second->instructions()[0].operands().size() != 3 ||
        commitFound->second->instructions()[1].opcode() != "br" ||
        commitFound->second->instructions()[1].operands().size() != 1)
      continue;
    const auto &commitStore = commitFound->second->instructions()[0];
    const std::string current = commitStore.operands()[1];
    const std::string targetAddress = commitStore.operands()[2];
    const std::string middleLatch =
        commitFound->second->instructions()[1].operands()[0];
    const auto currentPhi = definitions.find(current);
    if (currentPhi == definitions.end() ||
        currentPhi->second.block != header.name() ||
        currentPhi->second.instruction->opcode() != "phi")
      continue;

    std::optional<std::pair<std::string, std::string>> backedge;
    std::optional<std::pair<std::string, std::string>> initial;
    const auto &innerPhiOperands = innerPhiDef->second.instruction->operands();
    for (std::size_t index = 1; index + 1 < innerPhiOperands.size();
         index += 2) {
      const std::pair incoming{innerPhiOperands[index],
                               innerPhiOperands[index + 1]};
      if (isAddConstant(definitions, incoming.first, inner, 1))
        backedge = incoming;
      else
        initial = incoming;
    }
    if (!backedge.has_value() || !initial.has_value())
      continue;
    const auto backedgeFound = blocks.find(backedge->second);
    if (backedgeFound == blocks.end() ||
        backedgeFound->second->terminator() == nullptr ||
        backedgeFound->second->terminator()->operands() !=
            std::vector<std::string>{header.name()})
      continue;
    const std::string innerNext = backedge->first;
    const std::string initName = initial->second;
    const auto initFound = blocks.find(initName);
    if (initFound == blocks.end() ||
        initFound->second->terminator() == nullptr ||
        initFound->second->terminator()->operands() !=
            std::vector<std::string>{header.name()})
      continue;
    const auto initPreds = predecessors.find(initName);
    if (initPreds == predecessors.end() || initPreds->second.size() != 1)
      continue;
    const std::string guardName = initPreds->second.front();
    const auto guardFound = blocks.find(guardName);
    if (guardFound == blocks.end() ||
        guardFound->second->terminator() == nullptr ||
        guardFound->second->terminator()->opcode() != "br" ||
        guardFound->second->terminator()->operands().size() != 3 ||
        guardFound->second->terminator()->operands()[1] != initName ||
        guardFound->second->terminator()->operands()[2] != middleLatch)
      continue;
    const auto guardCondition =
        definitions.find(guardFound->second->terminator()->operands()[0]);
    if (guardCondition == definitions.end() ||
        guardCondition->second.block != guardName ||
        guardCondition->second.instruction->opcode() != "icmp.slt" ||
        guardCondition->second.instruction->operands() !=
            std::vector<std::string>{initial->first, middle})
      continue;
    const auto guardPreds = predecessors.find(guardName);
    if (guardPreds == predecessors.end() || guardPreds->second.size() != 1)
      continue;
    const std::string guardPreheader = guardPreds->second.front();
    const auto latchFound = blocks.find(middleLatch);
    if (latchFound == blocks.end() ||
        latchFound->second->instructions().empty() ||
        latchFound->second->instructions().front().opcode() == "phi")
      continue;
    const std::unordered_set<std::string> expectedLatchPreds{guardName,
                                                             commitName};
    const auto latchPreds = predecessors.find(middleLatch);
    if (latchPreds == predecessors.end() ||
        std::unordered_set<std::string>(latchPreds->second.begin(),
                                        latchPreds->second.end()) !=
            expectedLatchPreds)
      continue;

    const auto targetPath = accessPath(targetAddress, definitions);
    if (!targetPath.has_value() || targetPath->indices.size() != 2 ||
        targetPath->indices[1] != middle)
      continue;
    const std::string matrix = targetPath->root;
    const std::string outer = targetPath->indices[0];
    const auto targetDef = definitions.find(targetAddress);
    if (targetDef == definitions.end() ||
        !isGep(*targetDef->second.instruction) ||
        targetDef->second.instruction->operands().size() != 2)
      continue;
    const std::string rowBase = targetDef->second.instruction->operands()[0];
    const auto rowBaseDef = definitions.find(rowBase);
    if (rowBaseDef == definitions.end() ||
        !isGep(*rowBaseDef->second.instruction) ||
        rowBaseDef->second.instruction->operands() !=
            std::vector<std::string>{matrix, outer})
      continue;
    if (!isAddConstant(definitions, initial->first, outer, 1))
      continue;

    const Instruction *columnElement = nullptr;
    const Instruction *columnRow = nullptr;
    std::size_t columnLoadCount = 0;
    for (const auto &instruction : bodyFound->second->instructions()) {
      if (instruction.opcode() != "load" || instruction.operands().size() != 2)
        continue;
      const auto path = accessPath(instruction.operands()[1], definitions);
      if (!path.has_value() || path->root != matrix ||
          path->indices.size() != 2 || path->indices[0] != innerNext ||
          path->indices[1] != middle)
        continue;
      const auto elementDef = definitions.find(instruction.operands()[1]);
      if (elementDef == definitions.end() ||
          !isGep(*elementDef->second.instruction))
        continue;
      const auto rowDef =
          definitions.find(elementDef->second.instruction->operands()[0]);
      if (rowDef == definitions.end() || !isGep(*rowDef->second.instruction) ||
          rowDef->second.instruction->operands() !=
              std::vector<std::string>{matrix, innerNext})
        continue;
      ++columnLoadCount;
      columnElement = elementDef->second.instruction;
      columnRow = rowDef->second.instruction;
    }
    if (columnLoadCount != 1 || columnElement == nullptr ||
        columnRow == nullptr ||
        !onlyUse(function, columnRow->result(), columnElement->result()))
      continue;

    const auto middlePhi = definitions.find(middle);
    if (middlePhi == definitions.end() ||
        middlePhi->second.instruction->opcode() != "phi" ||
        middlePhi->second.instruction->operands().size() != 5)
      continue;
    const std::string middleHeader = middlePhi->second.block;
    const auto middleBackedge =
        phiIncoming(*middlePhi->second.instruction, middleLatch);
    const auto middleEntry =
        otherPhiIncoming(*middlePhi->second.instruction, middleLatch);
    if (!middleBackedge.has_value() || !middleEntry.has_value() ||
        !isAddConstant(definitions, middleBackedge->first, middle, 1) ||
        !isAddConstant(definitions, middleEntry->first, outer, 1))
      continue;
    const auto middleHeaderFound = blocks.find(middleHeader);
    if (middleHeaderFound == blocks.end() ||
        middleHeaderFound->second->terminator() == nullptr ||
        middleHeaderFound->second->terminator()->operands().size() != 3 ||
        middleHeaderFound->second->terminator()->operands()[0].empty())
      continue;
    const auto middleCondition = definitions.find(
        middleHeaderFound->second->terminator()->operands()[0]);
    if (middleCondition == definitions.end() ||
        middleCondition->second.block != middleHeader ||
        middleCondition->second.instruction->opcode() != "icmp.slt" ||
        middleCondition->second.instruction->operands().size() != 2 ||
        middleCondition->second.instruction->operands()[0] != middle)
      continue;
    long long boundLiteral = 0;
    if (!detail::parseIntegerLiteral(
            middleCondition->second.instruction->operands()[1], boundLiteral) ||
        boundLiteral <= 0)
      continue;
    const auto bound = static_cast<std::size_t>(boundLiteral);
    const auto extent = matrixExtent(module, function, matrix);
    constexpr std::size_t kMaximumShadowBytes = 16U * 1024U * 1024U;
    if (!extent.has_value() || bound > *extent ||
        bound > kMaximumShadowBytes / 4U / bound)
      continue;
    if (latchFound->second->terminator() == nullptr ||
        latchFound->second->terminator()->operands() !=
            std::vector<std::string>{middleHeader})
      continue;

    const std::string outerBody = middleEntry->second;
    const auto outerPhi = definitions.find(outer);
    if (outerPhi == definitions.end() ||
        outerPhi->second.instruction->opcode() != "phi" ||
        outerPhi->second.instruction->operands().size() != 5)
      continue;
    const std::string outerHeader = outerPhi->second.block;
    const auto outerHeaderFound = blocks.find(outerHeader);
    if (outerHeaderFound == blocks.end() ||
        outerHeaderFound->second->terminator() == nullptr ||
        outerHeaderFound->second->terminator()->operands().size() != 3 ||
        outerHeaderFound->second->terminator()->operands()[1] != outerBody)
      continue;
    const std::string outerLatch =
        middleHeaderFound->second->terminator()->operands()[2];
    const auto outerBackedge =
        phiIncoming(*outerPhi->second.instruction, outerLatch);
    const auto outerEntry =
        otherPhiIncoming(*outerPhi->second.instruction, outerLatch);
    long long outerInitial = 0;
    if (!outerBackedge.has_value() || !outerEntry.has_value() ||
        !isSubOne(definitions, outerBackedge->first, outer) ||
        !detail::parseIntegerLiteral(outerEntry->first, outerInitial) ||
        outerInitial != boundLiteral - 1)
      continue;
    const auto outerCondition =
        definitions.find(outerHeaderFound->second->terminator()->operands()[0]);
    if (outerCondition == definitions.end() ||
        outerCondition->second.block != outerHeader ||
        outerCondition->second.instruction->opcode() != "icmp.sge" ||
        outerCondition->second.instruction->operands() !=
            std::vector<std::string>{outer, "0"})
      continue;
    const auto outerLatchFound = blocks.find(outerLatch);
    if (outerLatchFound == blocks.end() ||
        outerLatchFound->second->terminator() == nullptr ||
        outerLatchFound->second->terminator()->operands() !=
            std::vector<std::string>{outerHeader})
      continue;
    const std::string outerExit =
        outerHeaderFound->second->terminator()->operands()[2];
    if (!reaches(blocks, middleHeaderFound->second->terminator()->operands()[1],
                 guardName, {middleLatch, outerHeader, outerExit}))
      continue;
    const auto region = outerRegion(blocks, outerBody, outerHeader, outerExit);
    if (!region.has_value())
      continue;
    bool effectsSafe = true;
    for (const auto &blockName : *region) {
      const auto *block = blocks.at(blockName);
      for (const auto &instruction : block->instructions()) {
        if (instruction.opcode() == "call") {
          effectsSafe = false;
          break;
        }
        if (instruction.opcode() != "store" ||
            instruction.operands().size() != 3)
          continue;
        const auto path = accessPath(instruction.operands()[2], definitions);
        if (path.has_value() && path->root == matrix &&
            path->indices == std::vector<std::string>{outer, middle})
          continue;
        const std::string root =
            path.has_value() ? path->root : instruction.operands()[2];
        const auto rootDefinition = definitions.find(root);
        const bool localObject =
            rootDefinition != definitions.end() &&
            rootDefinition->second.instruction->opcode() == "alloca";
        const bool distinctGlobal = !root.empty() && root.front() == '@' &&
                                    !matrix.empty() && matrix.front() == '@' &&
                                    root != matrix;
        if (!localObject && !distinctGlobal) {
          effectsSafe = false;
          break;
        }
      }
      if (!effectsSafe)
        break;
    }
    if (!effectsSafe)
      continue;

    return Candidate{functionIndex,
                     bound,
                     matrix,
                     outer,
                     middle,
                     innerNext,
                     current,
                     targetAddress,
                     rowBase,
                     outerBody,
                     innerBodyName,
                     columnRow->result(),
                     columnElement->result(),
                     guardPreheader,
                     guardName,
                     initName,
                     commitName,
                     middleLatch};
  }
  return std::nullopt;
}

std::string freshSymbol(const Module &module, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &global : module.globals())
    names.insert(global.name());
  for (const auto &function : module.functions())
    names.insert(function.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = stem + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem, bool value) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate =
        (value ? "%" : "") + stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

BasicBlock *findBlock(Function &function, const std::string &name) {
  for (auto &block : function.blocks())
    if (block.name() == name)
      return &block;
  return nullptr;
}

bool applyCandidate(Module &module, const Candidate &candidate) {
  const std::string shadowName =
      freshSymbol(module, "__triangular_transpose_shadow_");
  const std::string shadow = "@" + shadowName;
  const std::string type = "int[" + std::to_string(candidate.bound) + "][" +
                           std::to_string(candidate.bound) + "]";
  module.addGlobal(
      GlobalVariable{shadowName, type, Constant::aggregate(type, {})});

  auto &function = module.functions()[candidate.functionIndex];
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters())
    names.insert(parameter.name);
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
    for (const auto &instruction : block.instructions())
      if (instruction.hasResult())
        names.insert(instruction.result());
  }
  const auto mirrorName = freshName(names, "transpose.shadow.mirror", false);
  const auto diagonalAddress =
      freshName(names, "transpose.shadow.diagonal.address", true);
  const auto diagonalValue =
      freshName(names, "transpose.shadow.diagonal.value", true);
  const auto diagonalRow =
      freshName(names, "transpose.shadow.diagonal.row", true);
  const auto diagonalShadowAddress =
      freshName(names, "transpose.shadow.diagonal.destination", true);
  const auto shadowRow = freshName(names, "transpose.shadow.row", true);
  const auto commitAddress =
      freshName(names, "transpose.shadow.commit.address", true);
  const auto mirrorValue =
      freshName(names, "transpose.shadow.mirror.value", true);
  const auto mirrorAddress =
      freshName(names, "transpose.shadow.mirror.address", true);

  auto *outerBody = findBlock(function, candidate.outerBody);
  auto *innerBody = findBlock(function, candidate.innerBody);
  auto *guardPreheader = findBlock(function, candidate.guardPreheader);
  auto *init = findBlock(function, candidate.init);
  auto *commit = findBlock(function, candidate.commit);
  auto *guard = findBlock(function, candidate.guard);
  if (outerBody == nullptr || innerBody == nullptr ||
      guardPreheader == nullptr || init == nullptr || commit == nullptr ||
      guard == nullptr || outerBody->terminator() == nullptr ||
      guardPreheader->terminator() == nullptr ||
      init->terminator() == nullptr || commit->terminator() == nullptr ||
      guard->terminator() == nullptr)
    return false;

  auto outerInsert = outerBody->instructions().end() - 1;
  outerInsert = outerBody->instructions().insert(
      outerInsert, Instruction{"getelementptr",
                               {candidate.rowBase, candidate.outer},
                               diagonalAddress});
  ++outerInsert;
  outerInsert = outerBody->instructions().insert(
      outerInsert,
      Instruction{"load", {"int", diagonalAddress}, diagonalValue});
  ++outerInsert;
  outerInsert = outerBody->instructions().insert(
      outerInsert,
      Instruction{"getelementptr", {shadow, candidate.outer}, diagonalRow});
  ++outerInsert;
  outerInsert = outerBody->instructions().insert(
      outerInsert, Instruction{"getelementptr",
                               {diagonalRow, candidate.outer},
                               diagonalShadowAddress});
  ++outerInsert;
  outerBody->instructions().insert(
      outerInsert,
      Instruction{"store", {"int", diagonalValue, diagonalShadowAddress}});

  guardPreheader->instructions().insert(
      guardPreheader->instructions().end() - 1,
      Instruction{"getelementptr", {shadow, candidate.middle}, shadowRow});

  std::vector<Instruction> rewrittenBody;
  rewrittenBody.reserve(innerBody->instructions().size() - 1);
  for (const auto &instruction : innerBody->instructions()) {
    if (instruction.result() == candidate.columnRowGep)
      continue;
    if (instruction.result() == candidate.columnElementGep) {
      rewrittenBody.emplace_back(
          instruction.opcode(),
          std::vector<std::string>{shadowRow, candidate.innerNext},
          instruction.result());
    } else {
      rewrittenBody.push_back(instruction);
    }
  }
  innerBody->instructions() = std::move(rewrittenBody);

  auto commitInsert = commit->instructions().end() - 1;
  commitInsert = commit->instructions().insert(
      commitInsert, Instruction{"getelementptr",
                                {shadowRow, candidate.outer},
                                commitAddress});
  ++commitInsert;
  commit->instructions().insert(
      commitInsert,
      Instruction{"store", {"int", candidate.current, commitAddress}});

  auto guardOperands = guard->terminator()->operands();
  guardOperands[2] = mirrorName;
  guard->instructions().back() = Instruction{"br", std::move(guardOperands)};

  BasicBlock mirror{mirrorName};
  mirror.addInstruction(
      Instruction{"load", {"int", candidate.targetAddress}, mirrorValue});
  mirror.addInstruction(Instruction{
      "getelementptr", {shadowRow, candidate.outer}, mirrorAddress});
  mirror.addInstruction(
      Instruction{"store", {"int", mirrorValue, mirrorAddress}});
  mirror.addInstruction(Instruction{"br", {candidate.middleLatch}});
  function.addBlock(std::move(mirror));
  return true;
}

} // namespace

void TriangularTransposeShadowPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_TRIANGULAR_TRANSPOSE_SHADOW") != nullptr)
    return;
  std::size_t transformed = 0;
  for (std::size_t index = 0; index < module.functions().size(); ++index) {
    const auto candidate = matchFunction(module, index);
    if (candidate.has_value() && applyCandidate(module, *candidate)) {
      transformed = 1;
      break;
    }
  }
  if (std::getenv("COMPILER_TRIANGULAR_TRANSPOSE_SHADOW_STATS") != nullptr)
    std::cerr << "triangular-transpose-shadow: transformed=" << transformed
              << '\n';
}

} // namespace compiler::midend::pass
