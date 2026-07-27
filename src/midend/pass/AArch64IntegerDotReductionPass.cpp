#include "../../../include/compiler/midend/pass/AArch64IntegerDotReductionPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr int kVectorWidth = 8;
constexpr std::size_t kMaximumReductionsPerFunction = 16;

struct Candidate {
  std::string header;
  std::string body;
  std::string preheader;
  std::string induction;
  std::string accumulator;
  std::string inductionBackedge;
  std::string accumulatorBackedge;
  std::string initialInduction;
  std::string initialAccumulator;
  std::string bound;
  std::string leftBase;
  std::string rightBase;
};

bool isGep(const Instruction &instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "gep";
}

std::unordered_map<std::string, const Instruction *>
definitionsOf(const Function &function) {
  std::unordered_map<std::string, const Instruction *> definitions;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
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
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      if (auto found = predecessors.find(operands[index]);
          found != predecessors.end()) {
        found->second.push_back(block.name());
      }
    }
  }
  return predecessors;
}

std::unordered_map<std::string, std::size_t>
useCountsOf(const Function &function) {
  std::unordered_map<std::string, std::size_t> uses;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      for (const auto &operand : instruction.operands()) {
        if (!operand.empty() && operand.front() == '%')
          ++uses[operand];
      }
    }
  }
  return uses;
}

std::optional<std::pair<std::string, std::string>>
phiIncoming(const Instruction &phi, const std::string &predecessor) {
  if (phi.opcode() != "phi")
    return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) {
      return std::pair<std::string, std::string>{phi.operands()[index],
                                                 predecessor};
    }
  }
  return std::nullopt;
}

bool isAddOne(const Instruction *instruction, const std::string &value) {
  return instruction != nullptr && instruction->opcode() == "add" &&
         instruction->operands().size() == 2 &&
         ((instruction->operands()[0] == value &&
           instruction->operands()[1] == "1") ||
          (instruction->operands()[1] == value &&
           instruction->operands()[0] == "1"));
}

bool functionHasFloatingPoint(const Function &function) {
  if (function.returnType() == "float")
    return true;
  for (const auto &parameter : function.parameters()) {
    if (parameter.type == "float")
      return true;
  }
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if ((!instruction.opcode().empty() &&
           instruction.opcode().front() == 'f') ||
          std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    "float") != instruction.operands().end()) {
        return true;
      }
    }
  }
  return false;
}

bool vectorRegistersAreCompatible(const Function &function) {
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      const auto &opcode = instruction.opcode();
      if (opcode.rfind("aarch64.neon.", 0) != 0)
        continue;
      if (opcode == "aarch64.neon.dup.i32x4" ||
          opcode == "aarch64.neon.mls.i32x4" ||
          opcode == "aarch64.neon.load.acc.i32x4" ||
          opcode == "aarch64.neon.mls.acc.i32x4" ||
          opcode == "aarch64.neon.store.acc.i32x4" ||
          opcode == "aarch64.neon.load.acc.i32x8" ||
          opcode == "aarch64.neon.mls.acc.i32x8" ||
          opcode == "aarch64.neon.store.acc.i32x8" ||
          opcode == "aarch64.neon.zero.acc.i32x32" ||
          opcode == "aarch64.neon.dup.acc.i32x32" ||
          opcode == "aarch64.neon.mla.acc.i32x32" ||
          opcode == "aarch64.neon.store.acc.i32x32" ||
          opcode.rfind("aarch64.neon.reduce.mls.i32x8.", 0) == 0) {
        continue;
      }
      return false;
    }
  }
  return true;
}

const Instruction *definitionOf(
    const std::unordered_map<std::string, const Instruction *> &definitions,
    const std::string &value) {
  const auto found = definitions.find(value);
  return found == definitions.end() ? nullptr : found->second;
}

bool isTerminalContiguousLoad(
    const Instruction &load, const std::string &induction,
    const std::unordered_map<std::string, const Instruction *> &definitions,
    const std::unordered_map<std::string, std::size_t> &uses,
    std::string &base) {
  if (load.opcode() != "load" || load.operands().size() != 2 ||
      load.operands()[0] != "int") {
    return false;
  }
  const auto *gep = definitionOf(definitions, load.operands()[1]);
  if (gep == nullptr || !isGep(*gep) || gep->operands().size() != 2 ||
      gep->operands()[1] != induction) {
    return false;
  }
  const auto gepUses = uses.find(gep->result());
  if (gepUses == uses.end() || gepUses->second != 1)
    return false;
  base = gep->operands()[0];
  return true;
}

std::optional<Candidate> matchCandidate(const Function &function) {
  if (functionHasFloatingPoint(function) ||
      !vectorRegistersAreCompatible(function)) {
    return std::nullopt;
  }

  const auto definitions = definitionsOf(function);
  const auto blocks = blocksOf(function);
  const auto predecessors = predecessorsOf(function);
  const auto uses = useCountsOf(function);

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

    const auto predecessorFound = predecessors.find(header.name());
    if (predecessorFound == predecessors.end() ||
        predecessorFound->second.size() != 2) {
      continue;
    }
    const std::string bodyName = headerInstructions[3].operands()[1];
    const auto bodyFound = blocks.find(bodyName);
    if (bodyFound == blocks.end())
      continue;
    const auto *body = bodyFound->second;
    if (body->terminator() == nullptr || body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    const auto bodyPredecessors = predecessors.find(bodyName);
    if (bodyPredecessors == predecessors.end() ||
        bodyPredecessors->second != std::vector<std::string>{header.name()}) {
      continue;
    }

    std::string preheaderName;
    for (const auto &predecessor : predecessorFound->second) {
      if (predecessor != bodyName)
        preheaderName = predecessor;
    }
    const auto preheaderFound = blocks.find(preheaderName);
    if (preheaderFound == blocks.end() ||
        preheaderFound->second->terminator() == nullptr ||
        preheaderFound->second->terminator()->opcode() != "br" ||
        preheaderFound->second->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const std::string compared = headerInstructions[2].operands()[0];
    const Instruction *inductionPhi = nullptr;
    const Instruction *accumulatorPhi = nullptr;
    for (std::size_t index = 0; index < 2; ++index) {
      const auto &phi = headerInstructions[index];
      if (phi.result() == compared) {
        inductionPhi = &phi;
      } else {
        accumulatorPhi = &phi;
      }
    }
    if (inductionPhi == nullptr || accumulatorPhi == nullptr)
      continue;

    const auto inductionInitial = phiIncoming(*inductionPhi, preheaderName);
    const auto inductionBackedge = phiIncoming(*inductionPhi, bodyName);
    const auto accumulatorInitial = phiIncoming(*accumulatorPhi, preheaderName);
    const auto accumulatorBackedge = phiIncoming(*accumulatorPhi, bodyName);
    if (!inductionInitial.has_value() || !inductionBackedge.has_value() ||
        !accumulatorInitial.has_value() || !accumulatorBackedge.has_value() ||
        !isAddOne(definitionOf(definitions, inductionBackedge->first),
                  inductionPhi->result())) {
      continue;
    }

    const auto *subtract =
        definitionOf(definitions, accumulatorBackedge->first);
    if (subtract == nullptr || subtract->opcode() != "sub" ||
        subtract->operands().size() != 2 ||
        subtract->operands()[0] != accumulatorPhi->result()) {
      continue;
    }
    const auto *multiply = definitionOf(definitions, subtract->operands()[1]);
    if (multiply == nullptr || multiply->opcode() != "mul" ||
        multiply->operands().size() != 2) {
      continue;
    }
    const auto *leftLoad = definitionOf(definitions, multiply->operands()[0]);
    const auto *rightLoad = definitionOf(definitions, multiply->operands()[1]);
    if (leftLoad == nullptr || rightLoad == nullptr || leftLoad == rightLoad) {
      continue;
    }

    std::string leftBase;
    std::string rightBase;
    if (!isTerminalContiguousLoad(*leftLoad, inductionPhi->result(),
                                  definitions, uses, leftBase) ||
        !isTerminalContiguousLoad(*rightLoad, inductionPhi->result(),
                                  definitions, uses, rightBase)) {
      continue;
    }

    const std::unordered_set<const Instruction *> required{
        definitionOf(definitions, leftLoad->operands()[1]),
        leftLoad,
        definitionOf(definitions, rightLoad->operands()[1]),
        rightLoad,
        multiply,
        subtract,
        definitionOf(definitions, inductionBackedge->first),
        body->terminator()};
    bool exactReadOnlyLoop = true;
    for (const auto &instruction : body->instructions()) {
      if (required.find(&instruction) == required.end()) {
        exactReadOnlyLoop = false;
        break;
      }
    }
    if (!exactReadOnlyLoop || required.size() != body->instructions().size()) {
      continue;
    }

    const auto leftUses = uses.find(leftLoad->result());
    const auto rightUses = uses.find(rightLoad->result());
    const auto multiplyUses = uses.find(multiply->result());
    if (leftUses == uses.end() || leftUses->second != 1 ||
        rightUses == uses.end() || rightUses->second != 1 ||
        multiplyUses == uses.end() || multiplyUses->second != 1) {
      continue;
    }

    return Candidate{header.name(),
                     bodyName,
                     preheaderName,
                     inductionPhi->result(),
                     accumulatorPhi->result(),
                     inductionBackedge->first,
                     accumulatorBackedge->first,
                     inductionInitial->first,
                     accumulatorInitial->first,
                     headerInstructions[2].operands()[1],
                     leftBase,
                     rightBase};
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectValueNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters())
    names.insert(parameter.name);
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        names.insert(instruction.result());
    }
  }
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
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

BasicBlock *blockByName(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name)
      return &block;
  }
  return nullptr;
}

Instruction rewritePhiPredecessorAndAppend(
    const Instruction &phi, const std::string &oldPredecessor,
    const std::string &fallbackPredecessor, const std::string &vectorValue,
    const std::string &vectorPredecessor) {
  auto operands = phi.operands();
  for (std::size_t index = 2; index < operands.size(); index += 2) {
    if (operands[index] == oldPredecessor) {
      operands[index] = fallbackPredecessor;
    }
  }
  operands.push_back(vectorValue);
  operands.push_back(vectorPredecessor);
  return Instruction{"phi", std::move(operands), phi.result()};
}

bool transform(Function &function, const Candidate &candidate) {
  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  auto *preheader = blockByName(function, candidate.preheader);
  auto *header = blockByName(function, candidate.header);
  if (preheader == nullptr || header == nullptr ||
      preheader->terminator() == nullptr ||
      preheader->terminator()->operands() !=
          std::vector<std::string>{candidate.header}) {
    return false;
  }

  const std::string guardName = freshName(blockNames, "dot.vector.guard");
  const std::string prepareName = freshName(blockNames, "dot.vector.prepare");
  const std::string vectorHeaderName =
      freshName(blockNames, "dot.vector.header");
  const std::string vectorBodyName = freshName(blockNames, "dot.vector.body");
  const std::string finishName = freshName(blockNames, "dot.vector.finish");

  const std::string startNonnegative =
      freshName(valueNames, "%dot.vector.start.nonnegative");
  const std::string ordered = freshName(valueNames, "%dot.vector.ordered");
  const std::string remaining = freshName(valueNames, "%dot.vector.remaining");
  const std::string enough = freshName(valueNames, "%dot.vector.enough");
  const std::string safeOrder = freshName(valueNames, "%dot.vector.safe.order");
  const std::string canEnter = freshName(valueNames, "%dot.vector.can.enter");
  const std::string vectorTrips = freshName(valueNames, "%dot.vector.trips");
  const std::string vectorEnd = freshName(valueNames, "%dot.vector.end");
  const std::string vectorIndex = freshName(valueNames, "%dot.vector.index");
  const std::string vectorHasWork =
      freshName(valueNames, "%dot.vector.has.work");
  const std::string nextIndex = freshName(valueNames, "%dot.vector.next");
  const std::string vectorResult = freshName(valueNames, "%dot.vector.result");

  preheader->instructions().back() = Instruction{"br", {guardName}};

  BasicBlock guard{guardName};
  std::string remainingValue = remaining;
  std::string guardValue = canEnter;
  if (candidate.initialInduction == "0") {
    remainingValue = candidate.bound;
    guardValue = enough;
    guard.addInstruction(Instruction{
        "icmp.sge", {candidate.bound, std::to_string(kVectorWidth)}, enough});
  } else {
    guard.addInstruction(Instruction{
        "icmp.sge", {candidate.initialInduction, "0"}, startNonnegative});
    guard.addInstruction(Instruction{
        "icmp.sge", {candidate.bound, candidate.initialInduction}, ordered});
    guard.addInstruction(Instruction{
        "sub", {candidate.bound, candidate.initialInduction}, remaining});
    guard.addInstruction(Instruction{
        "icmp.sge", {remaining, std::to_string(kVectorWidth)}, enough});
    guard.addInstruction(
        Instruction{"and", {startNonnegative, ordered}, safeOrder});
    guard.addInstruction(Instruction{"and", {safeOrder, enough}, canEnter});
  }
  guard.addInstruction(
      Instruction{"br", {guardValue, prepareName, candidate.header}});

  BasicBlock prepare{prepareName};
  prepare.addInstruction(
      Instruction{"and", {remainingValue, "-8"}, vectorTrips});
  prepare.addInstruction(
      Instruction{"add", {candidate.initialInduction, vectorTrips}, vectorEnd});
  prepare.addInstruction(Instruction{"aarch64.neon.reduce.mls.i32x8.init",
                                     {candidate.initialAccumulator}});
  prepare.addInstruction(Instruction{"br", {vectorHeaderName}});

  BasicBlock vectorHeader{vectorHeaderName};
  vectorHeader.addInstruction(
      Instruction{"phi",
                  {"int", candidate.initialInduction, prepareName, nextIndex,
                   vectorBodyName},
                  vectorIndex});
  vectorHeader.addInstruction(
      Instruction{"icmp.slt", {vectorIndex, vectorEnd}, vectorHasWork});
  vectorHeader.addInstruction(
      Instruction{"br", {vectorHasWork, vectorBodyName, finishName}});

  BasicBlock vectorBody{vectorBodyName};
  vectorBody.addInstruction(
      Instruction{"aarch64.neon.reduce.mls.i32x8.step",
                  {candidate.leftBase, candidate.rightBase, vectorIndex}});
  vectorBody.addInstruction(Instruction{
      "add", {vectorIndex, std::to_string(kVectorWidth)}, nextIndex});
  vectorBody.addInstruction(Instruction{"br", {vectorHeaderName}});

  BasicBlock finish{finishName};
  finish.addInstruction(
      Instruction{"aarch64.neon.reduce.mls.i32x8.finish", {}, vectorResult});
  finish.addInstruction(Instruction{"br", {candidate.header}});

  for (std::size_t index = 0; index < 2; ++index) {
    const auto &phi = header->instructions()[index];
    if (phi.result() == candidate.induction) {
      header->instructions()[index] = rewritePhiPredecessorAndAppend(
          phi, candidate.preheader, guardName, vectorEnd, finishName);
    } else if (phi.result() == candidate.accumulator) {
      header->instructions()[index] = rewritePhiPredecessorAndAppend(
          phi, candidate.preheader, guardName, vectorResult, finishName);
    } else {
      return false;
    }
  }

  auto position =
      std::find_if(function.blocks().begin(), function.blocks().end(),
                   [&](const BasicBlock &block) {
                     return block.name() == candidate.header;
                   });
  if (position == function.blocks().end())
    return false;
  position = function.blocks().insert(position, std::move(guard));
  ++position;
  position = function.blocks().insert(position, std::move(prepare));
  ++position;
  position = function.blocks().insert(position, std::move(vectorHeader));
  ++position;
  position = function.blocks().insert(position, std::move(vectorBody));
  ++position;
  function.blocks().insert(position, std::move(finish));
  return true;
}

} // namespace

void AArch64IntegerDotReductionPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_INTEGER_DOT_REDUCTION") !=
      nullptr) {
    return;
  }
  for (auto &function : module.functions()) {
    for (std::size_t count = 0; count < kMaximumReductionsPerFunction;
         ++count) {
      const auto candidate = matchCandidate(function);
      if (!candidate.has_value() || !transform(function, *candidate))
        break;
    }
  }
}

} // namespace compiler::midend::pass
