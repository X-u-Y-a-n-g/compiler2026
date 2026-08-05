#include "../../../include/compiler/midend/pass/AArch64OrderedMemoryRecurrenceVectorizationPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

constexpr int kVectorWidth = 4;
constexpr int kMaximumVectorSlots = 14;
constexpr std::size_t kMaximumTransformsPerFunction = 8;

struct Definition {
  const Instruction *instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct LoadStream {
  const Instruction *load = nullptr;
  std::string base;
  std::int32_t offset = 0;
  int slot = 0;
};

struct VectorOperation {
  int opcode = 0;
  int destination = 0;
  bool lhsImmediate = false;
  std::int32_t lhs = 0;
  bool rhsImmediate = false;
  std::int32_t rhs = 0;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string preheader;
  std::string exit;
  std::string induction;
  std::string state;
  std::string stateBackedge;
  std::string bound;
  std::vector<LoadStream> streams;
  std::vector<VectorOperation> operations;
  int thresholdSlot = 0;
  int replacementSlot = 0;
};

bool hasConflictingVectorState(const Function &function) {
  if (function.returnType().find("float") != std::string::npos)
    return true;
  for (const auto &parameter : function.parameters()) {
    if (parameter.type.find("float") != std::string::npos)
      return true;
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &opcode = instruction.opcode();
      if (opcode == "fneg" || opcode == "fadd" || opcode == "fsub" ||
          opcode == "fmul" || opcode == "fdiv" || opcode == "sitofp" ||
          opcode == "uitofp" || opcode == "fptosi" ||
          opcode.rfind("fcmp.", 0) == 0 ||
          opcode.rfind("aarch64.neon.", 0) == 0)
        return true;
      for (const auto &operand : instruction.operands()) {
        if (operand.find("float") != std::string::npos)
          return true;
      }
    }
  }
  return false;
}

DefinitionMap definitionsOf(const Function &function) {
  DefinitionMap definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
    }
  }
  return definitions;
}

std::unordered_map<std::string, const BasicBlock *>
blocksOf(const Function &function) {
  std::unordered_map<std::string, const BasicBlock *> blocks;
  for (const auto &block : function.blocks())
    blocks.emplace(block.name(), &block);
  return blocks;
}

std::unordered_map<std::string, std::vector<std::string>>
predecessorsOf(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto &block : function.blocks())
    predecessors.emplace(block.name(), std::vector<std::string>{});
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t begin = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = begin; index < operands.size(); ++index) {
      const auto found = predecessors.find(operands[index]);
      if (found != predecessors.end())
        found->second.push_back(block.name());
    }
  }
  return predecessors;
}

std::optional<std::string> phiIncoming(const Instruction &phi,
                                       const std::string &predecessor) {
  if (phi.opcode() != "phi")
    return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor)
      return phi.operands()[index];
  }
  return std::nullopt;
}

bool parseI32(const std::string &text, std::int32_t &value) {
  long long parsed = 0;
  if (!detail::parseIntegerLiteral(text, parsed) ||
      parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max())
    return false;
  value = static_cast<std::int32_t>(parsed);
  return true;
}

std::optional<int> binaryOpcode(const std::string &opcode) {
  if (opcode == "add")
    return 0;
  if (opcode == "sub")
    return 1;
  if (opcode == "mul")
    return 2;
  return std::nullopt;
}

std::optional<std::int32_t>
affineOffset(const std::string &value, const std::string &induction,
             const std::string &body, const DefinitionMap &definitions,
             std::unordered_set<std::string> &slice,
             std::unordered_set<std::string> &visiting) {
  if (value == induction)
    return 0;
  if (!visiting.insert(value).second)
    return std::nullopt;
  const auto found = definitions.find(value);
  if (found == definitions.end() || found->second.block != body ||
      found->second.instruction->operands().size() != 2) {
    visiting.erase(value);
    return std::nullopt;
  }
  const auto &instruction = *found->second.instruction;
  if (instruction.opcode() != "add" && instruction.opcode() != "sub") {
    visiting.erase(value);
    return std::nullopt;
  }
  std::int32_t literal = 0;
  std::optional<std::int32_t> offset;
  if (parseI32(instruction.operands()[1], literal)) {
    const auto base = affineOffset(instruction.operands()[0], induction, body,
                                   definitions, slice, visiting);
    if (base.has_value()) {
      const std::int64_t combined =
          static_cast<std::int64_t>(*base) +
          (instruction.opcode() == "add" ? literal : -literal);
      if (combined >= std::numeric_limits<std::int32_t>::min() &&
          combined <= std::numeric_limits<std::int32_t>::max())
        offset = static_cast<std::int32_t>(combined);
    }
  } else if (instruction.opcode() == "add" &&
             parseI32(instruction.operands()[0], literal)) {
    const auto base = affineOffset(instruction.operands()[1], induction, body,
                                   definitions, slice, visiting);
    if (base.has_value()) {
      const std::int64_t combined = static_cast<std::int64_t>(*base) + literal;
      if (combined >= std::numeric_limits<std::int32_t>::min() &&
          combined <= std::numeric_limits<std::int32_t>::max())
        offset = static_cast<std::int32_t>(combined);
    }
  }
  visiting.erase(value);
  if (offset.has_value())
    slice.insert(value);
  return offset;
}

bool matchThresholdSelect(const Instruction &select,
                          const DefinitionMap &definitions,
                          const std::string &body, const std::string &state,
                          std::string &threshold, std::string &replacement,
                          const Instruction *&compare) {
  if (select.opcode() != "select" || select.operands().size() != 3)
    return false;
  const auto found = definitions.find(select.operands()[0]);
  if (found == definitions.end() || found->second.block != body ||
      found->second.instruction->operands().size() != 2)
    return false;
  compare = found->second.instruction;
  const auto &lhs = compare->operands()[0];
  const auto &rhs = compare->operands()[1];
  const auto &onTrue = select.operands()[1];
  const auto &onFalse = select.operands()[2];
  if (onFalse == state &&
      ((compare->opcode() == "icmp.slt" && lhs == state) ||
       (compare->opcode() == "icmp.sgt" && rhs == state))) {
    threshold = lhs == state ? rhs : lhs;
    replacement = onTrue;
    return threshold != state && replacement != state;
  }
  if (onTrue == state &&
      ((compare->opcode() == "icmp.sge" && lhs == state) ||
       (compare->opcode() == "icmp.sle" && rhs == state))) {
    threshold = lhs == state ? rhs : lhs;
    replacement = onFalse;
    return threshold != state && replacement != state;
  }
  return false;
}

bool collectMapSlice(const std::string &value, const std::string &body,
                     const std::string &state, const std::string &induction,
                     const DefinitionMap &definitions,
                     std::unordered_set<std::string> &slice,
                     std::unordered_set<std::string> &loads) {
  if (value == state || value == induction)
    return false;
  std::int32_t immediate = 0;
  if (parseI32(value, immediate))
    return true;
  if (!slice.insert(value).second)
    return true;
  const auto found = definitions.find(value);
  if (found == definitions.end() || found->second.block != body)
    return false;
  const auto &instruction = *found->second.instruction;
  if (instruction.opcode() == "load" && instruction.operands().size() == 2 &&
      instruction.operands()[0] == "int") {
    loads.insert(value);
    return true;
  }
  if (!binaryOpcode(instruction.opcode()).has_value() ||
      instruction.operands().size() != 2)
    return false;
  return collectMapSlice(instruction.operands()[0], body, state, induction,
                         definitions, slice, loads) &&
         collectMapSlice(instruction.operands()[1], body, state, induction,
                         definitions, slice, loads);
}

std::optional<Candidate> matchCandidate(const Function &function) {
  if (hasConflictingVectorState(function))
    return std::nullopt;
  const auto definitions = definitionsOf(function);
  const auto blocks = blocksOf(function);
  const auto predecessors = predecessorsOf(function);
  for (const auto &header : function.blocks()) {
    const auto &instructions = header.instructions();
    if (instructions.size() != 4 || instructions[0].opcode() != "phi" ||
        instructions[1].opcode() != "phi" ||
        instructions[2].opcode() != "icmp.slt" ||
        instructions[2].operands().size() != 2 ||
        instructions[3].opcode() != "br" ||
        instructions[3].operands().size() != 3 ||
        instructions[3].operands()[0] != instructions[2].result())
      continue;
    const std::string bodyName = instructions[3].operands()[1];
    const std::string exitName = instructions[3].operands()[2];
    const auto bodyFound = blocks.find(bodyName);
    const auto predFound = predecessors.find(header.name());
    if (bodyFound == blocks.end() || predFound == predecessors.end() ||
        predFound->second.size() != 2)
      continue;
    const auto *body = bodyFound->second;
    if (body->terminator() == nullptr || body->terminator()->opcode() != "br" ||
        body->terminator()->operands().size() != 1)
      continue;
    std::string latchName = bodyName;
    if (body->terminator()->operands()[0] != header.name()) {
      latchName = body->terminator()->operands()[0];
      const auto latchFound = blocks.find(latchName);
      if (latchFound == blocks.end() ||
          latchFound->second->instructions().size() != 1 ||
          latchFound->second->terminator() == nullptr ||
          latchFound->second->terminator()->opcode() != "br" ||
          latchFound->second->terminator()->operands() !=
              std::vector<std::string>{header.name()})
        continue;
    }

    std::string preheader;
    for (const auto &predecessor : predFound->second) {
      if (predecessor != latchName)
        preheader = predecessor;
    }
    const auto preheaderFound = blocks.find(preheader);
    if (preheader.empty() || preheaderFound == blocks.end() ||
        preheaderFound->second->terminator() == nullptr ||
        preheaderFound->second->terminator()->operands() !=
            std::vector<std::string>{header.name()})
      continue;

    const std::string compared = instructions[2].operands()[0];
    const Instruction *inductionPhi = nullptr;
    const Instruction *statePhi = nullptr;
    for (std::size_t index = 0; index < 2; ++index) {
      if (instructions[index].result() == compared)
        inductionPhi = &instructions[index];
      else
        statePhi = &instructions[index];
    }
    if (inductionPhi == nullptr || statePhi == nullptr)
      continue;
    const std::string bound = instructions[2].operands()[1];
    if (bound == inductionPhi->result() || bound == statePhi->result())
      continue;
    if (const auto found = definitions.find(bound);
        found != definitions.end() &&
        (found->second.block == header.name() || found->second.block == bodyName))
      continue;

    const auto inductionBackedge = phiIncoming(*inductionPhi, latchName);
    const auto stateBackedge = phiIncoming(*statePhi, latchName);
    if (!inductionBackedge.has_value() || !stateBackedge.has_value())
      continue;
    const auto updateFound = definitions.find(*inductionBackedge);
    if (updateFound == definitions.end() || updateFound->second.block != bodyName ||
        updateFound->second.instruction->opcode() != "add" ||
        updateFound->second.instruction->operands().size() != 2 ||
        !((updateFound->second.instruction->operands()[0] == inductionPhi->result() &&
           updateFound->second.instruction->operands()[1] == "1") ||
          (updateFound->second.instruction->operands()[1] == inductionPhi->result() &&
           updateFound->second.instruction->operands()[0] == "1")))
      continue;

    const auto selectFound = definitions.find(*stateBackedge);
    if (selectFound == definitions.end() || selectFound->second.block != bodyName)
      continue;
    std::string threshold;
    std::string replacement;
    const Instruction *compare = nullptr;
    if (!matchThresholdSelect(*selectFound->second.instruction, definitions,
                              bodyName, statePhi->result(), threshold,
                              replacement, compare))
      continue;

    std::unordered_set<std::string> mapSlice;
    std::unordered_set<std::string> loadValues;
    if (!collectMapSlice(threshold, bodyName, statePhi->result(),
                         inductionPhi->result(), definitions, mapSlice,
                         loadValues) ||
        !collectMapSlice(replacement, bodyName, statePhi->result(),
                         inductionPhi->result(), definitions, mapSlice,
                         loadValues) ||
        loadValues.empty() || loadValues.size() > 2)
      continue;

    std::vector<LoadStream> streams;
    std::unordered_set<std::string> addressSlice;
    bool streamsValid = true;
    for (const auto &instruction : body->instructions()) {
      if (loadValues.find(instruction.result()) == loadValues.end())
        continue;
      const auto addressFound = definitions.find(instruction.operands()[1]);
      if (addressFound == definitions.end() || addressFound->second.block != bodyName ||
          (addressFound->second.instruction->opcode() != "getelementptr" &&
           addressFound->second.instruction->opcode() != "getelementptr.linear" &&
           addressFound->second.instruction->opcode() != "gep" &&
           addressFound->second.instruction->opcode() != "gep.linear") ||
          addressFound->second.instruction->operands().size() != 2) {
        streamsValid = false;
        break;
      }
      const auto &gep = *addressFound->second.instruction;
      if (const auto baseDefinition = definitions.find(gep.operands()[0]);
          baseDefinition != definitions.end() &&
          (baseDefinition->second.block == header.name() ||
           baseDefinition->second.block == bodyName)) {
        streamsValid = false;
        break;
      }
      std::unordered_set<std::string> visiting;
      const auto offset = affineOffset(gep.operands()[1], inductionPhi->result(),
                                       bodyName, definitions, addressSlice,
                                       visiting);
      if (!offset.has_value()) {
        streamsValid = false;
        break;
      }
      addressSlice.insert(gep.result());
      streams.push_back(LoadStream{&instruction, gep.operands()[0], *offset,
                                   static_cast<int>(streams.size())});
    }
    if (!streamsValid || streams.size() != loadValues.size())
      continue;

    std::unordered_map<std::string, int> slots;
    for (const auto &stream : streams)
      slots.emplace(stream.load->result(), stream.slot);
    std::vector<VectorOperation> operations;
    int nextSlot = static_cast<int>(streams.size());
    bool operationsValid = true;
    for (const auto &instruction : body->instructions()) {
      if (mapSlice.find(instruction.result()) == mapSlice.end() ||
          loadValues.find(instruction.result()) != loadValues.end())
        continue;
      const auto opcode = binaryOpcode(instruction.opcode());
      if (!opcode.has_value() || nextSlot >= kMaximumVectorSlots) {
        operationsValid = false;
        break;
      }
      VectorOperation operation;
      operation.opcode = *opcode;
      operation.destination = nextSlot++;
      const auto encode = [&](const std::string &operand, bool &immediate,
                              std::int32_t &encoded) {
        if (const auto found = slots.find(operand); found != slots.end()) {
          immediate = false;
          encoded = found->second;
          return true;
        }
        immediate = true;
        return parseI32(operand, encoded);
      };
      if (!encode(instruction.operands()[0], operation.lhsImmediate,
                  operation.lhs) ||
          !encode(instruction.operands()[1], operation.rhsImmediate,
                  operation.rhs)) {
        operationsValid = false;
        break;
      }
      slots.emplace(instruction.result(), operation.destination);
      operations.push_back(operation);
    }
    const auto thresholdSlot = slots.find(threshold);
    const auto replacementSlot = slots.find(replacement);
    if (!operationsValid || thresholdSlot == slots.end() ||
        replacementSlot == slots.end())
      continue;

    std::unordered_set<std::string> allowedResults{*inductionBackedge,
                                                   *stateBackedge,
                                                   compare->result()};
    allowedResults.insert(mapSlice.begin(), mapSlice.end());
    allowedResults.insert(addressSlice.begin(), addressSlice.end());
    bool exactBody = true;
    for (const auto &instruction : body->instructions()) {
      if (instruction.isTerminator())
        continue;
      if (!instruction.hasResult() ||
          allowedResults.find(instruction.result()) == allowedResults.end()) {
        exactBody = false;
        break;
      }
    }
    if (!exactBody)
      continue;

    return Candidate{header.name(), bodyName, preheader, exitName,
                     inductionPhi->result(), statePhi->result(),
                     *stateBackedge, bound, std::move(streams),
                     std::move(operations), thresholdSlot->second,
                     replacementSlot->second};
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectValueNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters())
    names.insert(parameter.name);
  for (const auto &block : function.blocks())
    for (const auto &instruction : block.instructions())
      if (instruction.hasResult())
        names.insert(instruction.result());
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks())
    names.insert(block.name());
  return names;
}

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

BasicBlock *blockByName(Function &function, const std::string &name) {
  for (auto &block : function.blocks())
    if (block.name() == name)
      return &block;
  return nullptr;
}

Instruction appendPhiIncoming(const Instruction &phi, const std::string &value,
                              const std::string &predecessor) {
  auto operands = phi.operands();
  operands.push_back(value);
  operands.push_back(predecessor);
  return Instruction{"phi", std::move(operands), phi.result()};
}

bool transform(Function &function, const Candidate &candidate) {
  auto *header = blockByName(function, candidate.header);
  if (header == nullptr || header->instructions().size() != 4)
    return false;
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto guardName =
      freshName(blockNames, "ordered.memory.recurrence.vector.guard");
  const auto vectorHeaderName =
      freshName(blockNames, "ordered.memory.recurrence.vector.header");
  const auto vectorName =
      freshName(blockNames, "ordered.memory.recurrence.vector.body");

  header->instructions().back() =
      Instruction{"br", {header->instructions()[2].result(), guardName,
                          candidate.exit}};

  BasicBlock guard{guardName};
  const auto induction64 =
      freshName(valueNames, "%ordered.memory.recurrence.induction64");
  const auto bound64 =
      freshName(valueNames, "%ordered.memory.recurrence.bound64");
  const auto remaining64 =
      freshName(valueNames, "%ordered.memory.recurrence.remaining64");
  const auto vectorTrips64 =
      freshName(valueNames, "%ordered.memory.recurrence.vector.trips64");
  const auto vectorEnd64 =
      freshName(valueNames, "%ordered.memory.recurrence.vector.end64");
  const auto vectorEnd =
      freshName(valueNames, "%ordered.memory.recurrence.vector.end");
  const auto enough =
      freshName(valueNames, "%ordered.memory.recurrence.enough");
  guard.addInstruction(
      Instruction{"sext", {candidate.induction, "long"}, induction64});
  guard.addInstruction(Instruction{"sext", {candidate.bound, "long"}, bound64});
  guard.addInstruction(
      Instruction{"sub", {bound64, induction64}, remaining64});
  guard.addInstruction(
      Instruction{"and", {remaining64, "-4"}, vectorTrips64});
  guard.addInstruction(
      Instruction{"add", {induction64, vectorTrips64}, vectorEnd64});
  guard.addInstruction(
      Instruction{"trunc", {vectorEnd64, "int"}, vectorEnd});
  guard.addInstruction(Instruction{"icmp.sge", {remaining64, "4"}, enough});
  std::string valid = enough;
  for (const auto &stream : candidate.streams) {
    if (stream.offset >= 0 && stream.offset <= 1)
      continue;
    const auto last =
        freshName(valueNames, "%ordered.memory.recurrence.last.index");
    const auto first =
        freshName(valueNames, "%ordered.memory.recurrence.first.index");
    const auto lower =
        freshName(valueNames, "%ordered.memory.recurrence.index.lower");
    const auto upper =
        freshName(valueNames, "%ordered.memory.recurrence.index.upper");
    const auto fits =
        freshName(valueNames, "%ordered.memory.recurrence.index.fits");
    const auto combined =
        freshName(valueNames, "%ordered.memory.recurrence.valid");
    guard.addInstruction(Instruction{
        "add", {vectorEnd64, std::to_string(static_cast<std::int64_t>(stream.offset) - 1)},
        last});
    guard.addInstruction(Instruction{
        "add", {induction64, std::to_string(stream.offset)}, first});
    guard.addInstruction(
        Instruction{"icmp.sge", {first, "-2147483648"}, lower});
    guard.addInstruction(
        Instruction{"icmp.sle", {last, "2147483647"}, upper});
    guard.addInstruction(Instruction{"and", {lower, upper}, fits});
    guard.addInstruction(Instruction{"and", {valid, fits}, combined});
    valid = combined;
  }
  guard.addInstruction(
      Instruction{"br", {valid, vectorHeaderName, candidate.body}});

  const auto vectorInduction =
      freshName(valueNames, "%ordered.memory.recurrence.vector.induction");
  const auto vectorState =
      freshName(valueNames, "%ordered.memory.recurrence.vector.state");
  const auto vectorHasWork =
      freshName(valueNames, "%ordered.memory.recurrence.vector.has.work");
  const auto nextInduction =
      freshName(valueNames, "%ordered.memory.recurrence.induction.next");
  BasicBlock vectorHeader{vectorHeaderName};
  vectorHeader.addInstruction(Instruction{
      "phi", {"int", candidate.induction, guardName, nextInduction, vectorName},
      vectorInduction});

  BasicBlock vectorBody{vectorName};
  for (const auto &stream : candidate.streams) {
    std::string index = vectorInduction;
    if (stream.offset != 0) {
      index = freshName(valueNames, "%ordered.memory.recurrence.index");
      vectorBody.addInstruction(Instruction{
          "add", {vectorInduction, std::to_string(stream.offset)}, index});
    }
    vectorBody.addInstruction(Instruction{
        "aarch64.neon.pure.map.load.i32x4",
        {std::to_string(stream.slot), stream.base, index}});
  }
  for (const auto &operation : candidate.operations) {
    vectorBody.addInstruction(Instruction{
        "aarch64.neon.pure.map.binary.i32x4",
        {std::to_string(operation.opcode),
         std::to_string(operation.destination),
         operation.lhsImmediate ? "1" : "0", std::to_string(operation.lhs),
         operation.rhsImmediate ? "1" : "0", std::to_string(operation.rhs)}});
  }

  std::string currentState = vectorState;
  for (int lane = 0; lane < kVectorWidth; ++lane) {
    const auto threshold =
        freshName(valueNames, "%ordered.memory.recurrence.threshold");
    const auto replacement =
        freshName(valueNames, "%ordered.memory.recurrence.replacement");
    const auto condition =
        freshName(valueNames, "%ordered.memory.recurrence.condition");
    const auto nextState =
        freshName(valueNames, "%ordered.memory.recurrence.state");
    vectorBody.addInstruction(Instruction{
        "aarch64.neon.pure.map.extract.i32x4",
        {std::to_string(candidate.thresholdSlot), std::to_string(lane)},
        threshold});
    vectorBody.addInstruction(Instruction{
        "aarch64.neon.pure.map.extract.i32x4",
        {std::to_string(candidate.replacementSlot), std::to_string(lane)},
        replacement});
    vectorBody.addInstruction(
        Instruction{"icmp.slt", {currentState, threshold}, condition});
    vectorBody.addInstruction(
        Instruction{"select", {condition, replacement, currentState}, nextState});
    currentState = nextState;
  }
  vectorBody.addInstruction(
      Instruction{"add", {vectorInduction, "4"}, nextInduction});
  vectorBody.addInstruction(Instruction{"br", {vectorHeaderName}});

  vectorHeader.addInstruction(Instruction{
      "phi", {"int", candidate.state, guardName, currentState, vectorName},
      vectorState});
  vectorHeader.addInstruction(
      Instruction{"icmp.slt", {vectorInduction, vectorEnd}, vectorHasWork});
  vectorHeader.addInstruction(
      Instruction{"br", {vectorHasWork, vectorName, candidate.header}});

  for (std::size_t index = 0; index < 2; ++index) {
    const auto &phi = header->instructions()[index];
    if (phi.result() == candidate.induction)
      header->instructions()[index] =
          appendPhiIncoming(phi, vectorInduction, vectorHeaderName);
    else if (phi.result() == candidate.state)
      header->instructions()[index] =
          appendPhiIncoming(phi, vectorState, vectorHeaderName);
    else
      return false;
  }

  auto position = std::find_if(
      function.blocks().begin(), function.blocks().end(),
      [&](const BasicBlock &block) { return block.name() == candidate.body; });
  if (position == function.blocks().end())
    return false;
  position = function.blocks().insert(position, std::move(guard));
  ++position;
  position = function.blocks().insert(position, std::move(vectorHeader));
  ++position;
  function.blocks().insert(position, std::move(vectorBody));
  return true;
}

} // namespace

void AArch64OrderedMemoryRecurrenceVectorizationPass::run(Module &module) {
  if (std::getenv(
          "COMPILER_DISABLE_AARCH64_ORDERED_MEMORY_RECURRENCE_VECTOR") !=
      nullptr)
    return;
  bool transformed = false;
  for (auto &function : module.functions()) {
    for (std::size_t count = 0; count < kMaximumTransformsPerFunction; ++count) {
      const auto candidate = matchCandidate(function);
      if (!candidate.has_value() || !transform(function, *candidate))
        break;
      transformed = true;
    }
  }
  if (transformed)
    module.addPassNote(
        "vectorized contiguous read-only maps while preserving ordered scalar recurrence commits");
}

} // namespace compiler::midend::pass
