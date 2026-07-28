#include "../../../include/compiler/midend/pass/AArch64PureMapRecurrenceVectorizationPass.h"

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

struct Dependency {
  bool state = false;
  bool induction = false;
};

struct SignedMagic {
  std::int32_t multiplier = 0;
  unsigned shift = 0;
};

struct VectorOperation {
  enum class Kind {
    Binary,
    Divide,
    Remainder,
  };

  Kind kind = Kind::Binary;
  int opcode = 0;
  int destination = 0;
  bool lhsImmediate = false;
  std::int32_t lhs = 0;
  bool rhsImmediate = false;
  std::int32_t rhs = 0;
  std::int32_t divisor = 0;
  SignedMagic magic;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string preheader;
  std::string exit;
  std::string induction;
  std::string state;
  std::string inductionBackedge;
  std::string stateBackedge;
  std::string step;
  std::string mapRoot;
  int mapRootSlot = 0;
  std::vector<VectorOperation> vectorOperations;
  std::vector<const Instruction *> recurrenceInstructions;
};

DefinitionMap definitionsOf(const Function &function) {
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

std::unordered_map<std::string, const BasicBlock *>
blocksOf(const Function &function) {
  std::unordered_map<std::string, const BasicBlock *> blocks;
  for (const auto &block : function.blocks()) {
    blocks.emplace(block.name(), &block);
  }
  return blocks;
}

std::unordered_map<std::string, std::vector<std::string>>
predecessorsOf(const Function &function) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  for (const auto &block : function.blocks()) {
    predecessors.emplace(block.name(), std::vector<std::string>{});
  }
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto &operands = terminator->operands();
    const std::size_t begin = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = begin; index < operands.size(); ++index) {
      const auto found = predecessors.find(operands[index]);
      if (found != predecessors.end()) {
        found->second.push_back(block.name());
      }
    }
  }
  return predecessors;
}

std::optional<std::string> phiIncoming(const Instruction &phi,
                                       const std::string &predecessor) {
  if (phi.opcode() != "phi") {
    return std::nullopt;
  }
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) {
      return phi.operands()[index];
    }
  }
  return std::nullopt;
}

bool parseI32(const std::string &text, std::int32_t &value) {
  long long parsed = 0;
  if (!detail::parseIntegerLiteral(text, parsed) ||
      parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  value = static_cast<std::int32_t>(parsed);
  return true;
}

bool isValue(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

SignedMagic signedMagicForPositiveDivisor(std::uint32_t divisor) {
  const std::uint64_t two31 = std::uint64_t{1} << 31;
  const std::uint64_t anc = two31 - 1 - (two31 - 1) % divisor;
  unsigned p = 31;
  std::uint64_t q1 = two31 / anc;
  std::uint64_t r1 = two31 - q1 * anc;
  std::uint64_t q2 = two31 / divisor;
  std::uint64_t r2 = two31 - q2 * divisor;
  while (true) {
    ++p;
    q1 *= 2;
    r1 *= 2;
    if (r1 >= anc) {
      ++q1;
      r1 -= anc;
    }
    q2 *= 2;
    r2 *= 2;
    if (r2 >= divisor) {
      ++q2;
      r2 -= divisor;
    }
    const std::uint64_t delta = divisor - r2;
    if (!(q1 < delta || (q1 == delta && r1 == 0))) {
      break;
    }
  }
  return SignedMagic{static_cast<std::int32_t>(q2 + 1), p - 32};
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

std::optional<int> selectMinMaxOpcode(const Instruction &select,
                                      const DefinitionMap &definitions,
                                      std::string &lhs, std::string &rhs) {
  if (select.opcode() != "select" || select.operands().size() != 3) {
    return std::nullopt;
  }
  const auto condition = definitions.find(select.operands()[0]);
  if (condition == definitions.end() ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto &compare = *condition->second.instruction;
  lhs = compare.operands()[0];
  rhs = compare.operands()[1];
  const auto &onTrue = select.operands()[1];
  const auto &onFalse = select.operands()[2];
  if (compare.opcode() == "icmp.slt") {
    if (onTrue == rhs && onFalse == lhs)
      return 3;
    if (onTrue == lhs && onFalse == rhs)
      return 4;
  }
  if (compare.opcode() == "icmp.sgt") {
    if (onTrue == lhs && onFalse == rhs)
      return 3;
    if (onTrue == rhs && onFalse == lhs)
      return 4;
  }
  return std::nullopt;
}

bool isSupportedRecurrenceInstruction(const Instruction &instruction) {
  return instruction.opcode() == "add" || instruction.opcode() == "sub" ||
         instruction.opcode() == "mul" || instruction.opcode() == "sdiv" ||
         instruction.opcode() == "srem";
}

std::unordered_set<std::string>
backwardSlice(const std::string &root, const DefinitionMap &definitions,
              const std::string &body,
              const std::unordered_set<std::string> &stops) {
  std::unordered_set<std::string> result;
  std::vector<std::string> pending{root};
  while (!pending.empty()) {
    const auto value = std::move(pending.back());
    pending.pop_back();
    if (stops.find(value) != stops.end() || !result.insert(value).second) {
      continue;
    }
    const auto found = definitions.find(value);
    if (found == definitions.end() || found->second.block != body) {
      continue;
    }
    for (const auto &operand : found->second.instruction->operands()) {
      if (isValue(operand)) {
        pending.push_back(operand);
      }
    }
  }
  return result;
}

std::optional<Candidate> matchCandidate(const Function &function) {
  const auto definitions = definitionsOf(function);
  const auto blocks = blocksOf(function);
  const auto predecessors = predecessorsOf(function);

  for (const auto &header : function.blocks()) {
    const auto &headerInstructions = header.instructions();
    if (headerInstructions.size() != 4 ||
        headerInstructions[0].opcode() != "phi" ||
        headerInstructions[1].opcode() != "phi" ||
        headerInstructions[2].opcode() != "icmp.slt" ||
        headerInstructions[2].operands().size() != 2 ||
        headerInstructions[3].opcode() != "br" ||
        headerInstructions[3].operands().size() != 3 ||
        headerInstructions[3].operands()[0] != headerInstructions[2].result()) {
      continue;
    }
    const std::string bodyName = headerInstructions[3].operands()[1];
    const std::string exitName = headerInstructions[3].operands()[2];
    const auto bodyFound = blocks.find(bodyName);
    const auto predecessorFound = predecessors.find(header.name());
    if (bodyFound == blocks.end() || predecessorFound == predecessors.end() ||
        predecessorFound->second.size() != 2) {
      continue;
    }
    const auto *body = bodyFound->second;
    if (body->terminator() == nullptr || body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    std::string preheaderName;
    for (const auto &predecessor : predecessorFound->second) {
      if (predecessor != bodyName) {
        preheaderName = predecessor;
      }
    }
    const auto preheaderFound = blocks.find(preheaderName);
    if (preheaderName.empty() || preheaderFound == blocks.end() ||
        preheaderFound->second->terminator() == nullptr ||
        preheaderFound->second->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const std::string compared = headerInstructions[2].operands()[0];
    const Instruction *inductionPhi = nullptr;
    const Instruction *statePhi = nullptr;
    for (std::size_t index = 0; index < 2; ++index) {
      if (headerInstructions[index].result() == compared) {
        inductionPhi = &headerInstructions[index];
      } else {
        statePhi = &headerInstructions[index];
      }
    }
    if (inductionPhi == nullptr || statePhi == nullptr) {
      continue;
    }
    const std::string bound = headerInstructions[2].operands()[1];
    if (bound == inductionPhi->result() || bound == statePhi->result()) {
      continue;
    }
    if (isValue(bound)) {
      const auto boundDefinition = definitions.find(bound);
      if (boundDefinition != definitions.end() &&
          (boundDefinition->second.block == header.name() ||
           boundDefinition->second.block == bodyName)) {
        continue;
      }
    }
    const auto inductionBackedge = phiIncoming(*inductionPhi, bodyName);
    const auto stateBackedge = phiIncoming(*statePhi, bodyName);
    if (!inductionBackedge.has_value() || !stateBackedge.has_value()) {
      continue;
    }
    const auto inductionUpdate = definitions.find(*inductionBackedge);
    if (inductionUpdate == definitions.end() ||
        inductionUpdate->second.block != bodyName ||
        inductionUpdate->second.instruction->opcode() != "add" ||
        inductionUpdate->second.instruction->operands().size() != 2) {
      continue;
    }
    std::string step;
    if (inductionUpdate->second.instruction->operands()[0] ==
        inductionPhi->result()) {
      step = inductionUpdate->second.instruction->operands()[1];
    } else if (inductionUpdate->second.instruction->operands()[1] ==
               inductionPhi->result()) {
      step = inductionUpdate->second.instruction->operands()[0];
    } else {
      continue;
    }
    if (step == inductionPhi->result() || step == statePhi->result()) {
      continue;
    }
    if (isValue(step)) {
      const auto stepDefinition = definitions.find(step);
      if (stepDefinition != definitions.end() &&
          (stepDefinition->second.block == header.name() ||
           stepDefinition->second.block == bodyName)) {
        continue;
      }
    }

    std::unordered_map<std::string, Dependency> dependencies;
    dependencies.emplace(statePhi->result(), Dependency{true, false});
    dependencies.emplace(inductionPhi->result(), Dependency{false, true});
    bool dependencyFailure = false;
    for (const auto &instruction : body->instructions()) {
      if (!instruction.hasResult()) {
        continue;
      }
      Dependency dependency;
      for (const auto &operand : instruction.operands()) {
        const auto found = dependencies.find(operand);
        if (found != dependencies.end()) {
          dependency.state = dependency.state || found->second.state;
          dependency.induction =
              dependency.induction || found->second.induction;
        } else if (isValue(operand)) {
          const auto definition = definitions.find(operand);
          if (definition != definitions.end() &&
              definition->second.block == bodyName) {
            dependencyFailure = true;
          }
        }
      }
      dependencies[instruction.result()] = dependency;
    }
    if (dependencyFailure) {
      continue;
    }
    const auto finalDependency = dependencies.find(*stateBackedge);
    if (finalDependency == dependencies.end() ||
        !finalDependency->second.state || !finalDependency->second.induction) {
      continue;
    }

    std::unordered_set<std::string> recurrenceSlice;
    std::unordered_set<std::string> mapRoots;
    bool recurrenceSupported = true;
    std::vector<std::string> recurrencePending{*stateBackedge};
    while (!recurrencePending.empty()) {
      const auto value = std::move(recurrencePending.back());
      recurrencePending.pop_back();
      if (value == statePhi->result()) {
        continue;
      }
      const auto dependencyFound = dependencies.find(value);
      if (dependencyFound != dependencies.end() &&
          dependencyFound->second.induction && !dependencyFound->second.state) {
        mapRoots.insert(value);
        continue;
      }
      if (!recurrenceSlice.insert(value).second) {
        continue;
      }
      const auto found = definitions.find(value);
      if (found == definitions.end() || found->second.block != bodyName) {
        continue;
      }
      if (!isSupportedRecurrenceInstruction(*found->second.instruction)) {
        recurrenceSupported = false;
      }
      for (const auto &operand : found->second.instruction->operands()) {
        if (isValue(operand)) {
          recurrencePending.push_back(operand);
        }
      }
    }
    if (!recurrenceSupported || mapRoots.size() != 1) {
      continue;
    }
    const std::string mapRoot = *mapRoots.begin();
    const auto mapSlice =
        backwardSlice(mapRoot, definitions, bodyName, {inductionPhi->result()});

    std::unordered_map<std::string, int> slots;
    slots.emplace(inductionPhi->result(), 0);
    std::vector<VectorOperation> operations;
    int nextSlot = 1;
    bool mapSupported = true;
    const auto vectorOperand = [&](const std::string &operand, bool &immediate,
                                   std::int32_t &encoded) -> bool {
      const auto slot = slots.find(operand);
      if (slot != slots.end()) {
        immediate = false;
        encoded = slot->second;
        return true;
      }
      immediate = true;
      return parseI32(operand, encoded);
    };

    for (const auto &instruction : body->instructions()) {
      if (!instruction.hasResult() ||
          mapSlice.find(instruction.result()) == mapSlice.end()) {
        continue;
      }
      if (instruction.opcode().rfind("icmp.", 0) == 0) {
        continue;
      }
      if (nextSlot >= kMaximumVectorSlots) {
        mapSupported = false;
        break;
      }
      VectorOperation operation;
      operation.destination = nextSlot++;
      if (const auto opcode = binaryOpcode(instruction.opcode());
          opcode.has_value() && instruction.operands().size() == 2) {
        operation.kind = VectorOperation::Kind::Binary;
        operation.opcode = *opcode;
        if (!vectorOperand(instruction.operands()[0], operation.lhsImmediate,
                           operation.lhs) ||
            !vectorOperand(instruction.operands()[1], operation.rhsImmediate,
                           operation.rhs)) {
          mapSupported = false;
          break;
        }
      } else if (instruction.opcode() == "select") {
        std::string lhs;
        std::string rhs;
        const auto opcode =
            selectMinMaxOpcode(instruction, definitions, lhs, rhs);
        if (!opcode.has_value()) {
          mapSupported = false;
          break;
        }
        operation.kind = VectorOperation::Kind::Binary;
        operation.opcode = *opcode;
        if (!vectorOperand(lhs, operation.lhsImmediate, operation.lhs) ||
            !vectorOperand(rhs, operation.rhsImmediate, operation.rhs)) {
          mapSupported = false;
          break;
        }
      } else if ((instruction.opcode() == "sdiv" ||
                  instruction.opcode() == "srem") &&
                 instruction.operands().size() == 2) {
        std::int32_t divisor = 0;
        if (!parseI32(instruction.operands()[1], divisor) || divisor <= 1 ||
            !vectorOperand(instruction.operands()[0], operation.lhsImmediate,
                           operation.lhs) ||
            operation.lhsImmediate) {
          mapSupported = false;
          break;
        }
        operation.kind = instruction.opcode() == "sdiv"
                             ? VectorOperation::Kind::Divide
                             : VectorOperation::Kind::Remainder;
        operation.divisor = divisor;
        operation.magic =
            signedMagicForPositiveDivisor(static_cast<std::uint32_t>(divisor));
      } else {
        mapSupported = false;
        break;
      }
      slots.emplace(instruction.result(), operation.destination);
      operations.push_back(operation);
    }
    const auto rootSlot = slots.find(mapRoot);
    if (!mapSupported || rootSlot == slots.end() || operations.empty()) {
      continue;
    }

    std::unordered_set<std::string> allowedResults{*inductionBackedge};
    allowedResults.insert(mapSlice.begin(), mapSlice.end());
    allowedResults.insert(recurrenceSlice.begin(), recurrenceSlice.end());
    bool exactBody = true;
    for (const auto &instruction : body->instructions()) {
      if (instruction.isTerminator()) {
        continue;
      }
      if (!instruction.hasResult() ||
          allowedResults.find(instruction.result()) == allowedResults.end()) {
        exactBody = false;
        break;
      }
    }
    if (!exactBody) {
      continue;
    }

    std::vector<const Instruction *> recurrenceInstructions;
    for (const auto &instruction : body->instructions()) {
      if (instruction.hasResult() &&
          recurrenceSlice.find(instruction.result()) != recurrenceSlice.end() &&
          mapSlice.find(instruction.result()) == mapSlice.end()) {
        recurrenceInstructions.push_back(&instruction);
      }
    }
    if (recurrenceInstructions.empty() ||
        recurrenceInstructions.back()->result() != *stateBackedge) {
      continue;
    }

    return Candidate{header.name(),
                     bodyName,
                     preheaderName,
                     exitName,
                     inductionPhi->result(),
                     statePhi->result(),
                     *inductionBackedge,
                     *stateBackedge,
                     step,
                     mapRoot,
                     rootSlot->second,
                     std::move(operations),
                     std::move(recurrenceInstructions)};
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectValueNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

BasicBlock *blockByName(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
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
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  auto *header = blockByName(function, candidate.header);
  if (header == nullptr || header->instructions().size() != 4) {
    return false;
  }
  const auto guardName = freshName(blockNames, "pure.map.vector.guard");
  const auto vectorName = freshName(blockNames, "pure.map.vector.body");
  const auto x1 = freshName(valueNames, "%pure.map.x1");
  const auto x2 = freshName(valueNames, "%pure.map.x2");
  const auto x3 = freshName(valueNames, "%pure.map.x3");
  const auto x4 = freshName(valueNames, "%pure.map.x4");
  const auto c1 = freshName(valueNames, "%pure.map.c1");
  const auto c2 = freshName(valueNames, "%pure.map.c2");
  const auto c3 = freshName(valueNames, "%pure.map.c3");
  const auto c12 = freshName(valueNames, "%pure.map.c12");
  const auto all = freshName(valueNames, "%pure.map.all");
  std::vector<std::string> laneValues;
  for (int lane = 0; lane < kVectorWidth; ++lane) {
    laneValues.push_back(freshName(valueNames, "%pure.map.lane"));
  }

  header->instructions().back() = Instruction{
      "br", {header->instructions()[2].result(), guardName, candidate.exit}};

  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"add", {candidate.induction, candidate.step}, x1});
  guard.addInstruction(Instruction{"add", {x1, candidate.step}, x2});
  guard.addInstruction(Instruction{"add", {x2, candidate.step}, x3});
  guard.addInstruction(Instruction{
      "icmp.slt", {x1, header->instructions()[2].operands()[1]}, c1});
  guard.addInstruction(Instruction{
      "icmp.slt", {x2, header->instructions()[2].operands()[1]}, c2});
  guard.addInstruction(Instruction{
      "icmp.slt", {x3, header->instructions()[2].operands()[1]}, c3});
  guard.addInstruction(Instruction{"and", {c1, c2}, c12});
  guard.addInstruction(Instruction{"and", {c12, c3}, all});
  guard.addInstruction(Instruction{"br", {all, vectorName, candidate.body}});

  BasicBlock vectorBody{vectorName};
  vectorBody.addInstruction(Instruction{"aarch64.neon.pure.map.i32x4.init",
                                        {candidate.induction, x1, x2, x3}});
  for (const auto &operation : candidate.vectorOperations) {
    if (operation.kind == VectorOperation::Kind::Binary) {
      vectorBody.addInstruction(Instruction{
          "aarch64.neon.pure.map.binary.i32x4",
          {std::to_string(operation.opcode),
           std::to_string(operation.destination),
           operation.lhsImmediate ? "1" : "0", std::to_string(operation.lhs),
           operation.rhsImmediate ? "1" : "0", std::to_string(operation.rhs)}});
    } else {
      vectorBody.addInstruction(Instruction{
          operation.kind == VectorOperation::Kind::Divide
              ? "aarch64.neon.pure.map.sdiv.i32x4"
              : "aarch64.neon.pure.map.srem.i32x4",
          {std::to_string(operation.destination), std::to_string(operation.lhs),
           std::to_string(operation.divisor),
           std::to_string(operation.magic.multiplier),
           std::to_string(operation.magic.shift)}});
    }
  }
  for (int lane = 0; lane < kVectorWidth; ++lane) {
    vectorBody.addInstruction(Instruction{
        "aarch64.neon.pure.map.extract.i32x4",
        {std::to_string(candidate.mapRootSlot), std::to_string(lane)},
        laneValues[lane]});
  }

  std::string currentState = candidate.state;
  for (int lane = 0; lane < kVectorWidth; ++lane) {
    std::unordered_map<std::string, std::string> replacements{
        {candidate.state, currentState}, {candidate.mapRoot, laneValues[lane]}};
    for (const auto *instruction : candidate.recurrenceInstructions) {
      auto operands = instruction->operands();
      for (auto &operand : operands) {
        const auto found = replacements.find(operand);
        if (found != replacements.end()) {
          operand = found->second;
        }
      }
      const auto result = freshName(valueNames, "%pure.map.recurrence");
      replacements[instruction->result()] = result;
      vectorBody.addInstruction(
          Instruction{instruction->opcode(), std::move(operands), result});
    }
    currentState = replacements.at(candidate.stateBackedge);
  }
  vectorBody.addInstruction(Instruction{"add", {x3, candidate.step}, x4});
  vectorBody.addInstruction(Instruction{"br", {candidate.header}});

  for (std::size_t index = 0; index < 2; ++index) {
    const auto &phi = header->instructions()[index];
    if (phi.result() == candidate.induction) {
      header->instructions()[index] = appendPhiIncoming(phi, x4, vectorName);
    } else if (phi.result() == candidate.state) {
      header->instructions()[index] =
          appendPhiIncoming(phi, currentState, vectorName);
    } else {
      return false;
    }
  }

  auto position = std::find_if(
      function.blocks().begin(), function.blocks().end(),
      [&](const BasicBlock &block) { return block.name() == candidate.body; });
  if (position == function.blocks().end()) {
    return false;
  }
  position = function.blocks().insert(position, std::move(guard));
  ++position;
  function.blocks().insert(position, std::move(vectorBody));
  return true;
}

} // namespace

void AArch64PureMapRecurrenceVectorizationPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_PURE_MAP_RECURRENCE_VECTOR") !=
      nullptr) {
    return;
  }
  for (auto &function : module.functions()) {
    for (std::size_t count = 0; count < kMaximumTransformsPerFunction;
         ++count) {
      const auto candidate = matchCandidate(function);
      if (!candidate.has_value() || !transform(function, *candidate)) {
        break;
      }
    }
  }
}

} // namespace compiler::midend::pass
