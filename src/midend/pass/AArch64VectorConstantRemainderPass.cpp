#include "../../../include/compiler/midend/pass/AArch64VectorConstantRemainderPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr int kVectorWidth = 4;
constexpr std::size_t kMaximumTransformsPerFunction = 16;

struct Candidate {
  std::string header;
  std::string body;
  std::string preheader;
  std::string induction;
  std::string inductionBackedge;
  std::string initialInduction;
  std::string bound;
  std::string base;
};

BasicBlock *blockByName(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name)
      return &block;
  }
  return nullptr;
}

const BasicBlock *blockByName(const Function &function,
                              const std::string &name) {
  for (const auto &block : function.blocks()) {
    if (block.name() == name)
      return &block;
  }
  return nullptr;
}

std::optional<std::string> incomingValue(const Instruction &phi,
                                         const std::string &predecessor) {
  if (phi.opcode() != "phi")
    return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor)
      return phi.operands()[index];
  }
  return std::nullopt;
}

bool functionHasNeonState(const Function &function) {
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode().rfind("aarch64.neon.", 0) == 0)
        return true;
    }
  }
  return false;
}

std::optional<Candidate> matchCandidate(const Function &function) {
  if (functionHasNeonState(function))
    return std::nullopt;

  for (const auto &header : function.blocks()) {
    const auto &headerInstructions = header.instructions();
    if (headerInstructions.size() != 3 ||
        headerInstructions[0].opcode() != "phi" ||
        headerInstructions[1].opcode() != "icmp.slt" ||
        headerInstructions[1].operands().size() != 2 ||
        headerInstructions[1].operands()[0] !=
            headerInstructions[0].result() ||
        headerInstructions[2].opcode() != "br" ||
        headerInstructions[2].operands().size() != 3 ||
        headerInstructions[2].operands()[0] !=
            headerInstructions[1].result()) {
      continue;
    }

    const std::string &induction = headerInstructions[0].result();
    const std::string &bodyName = headerInstructions[2].operands()[1];
    const auto *body = blockByName(function, bodyName);
    if (body == nullptr)
      continue;
    const auto &instructions = body->instructions();
    if (instructions.size() != 6 ||
        (instructions[0].opcode() != "getelementptr" &&
         instructions[0].opcode() != "gep") ||
        instructions[0].operands().size() != 2 ||
        instructions[0].operands()[1] != induction ||
        instructions[1].opcode() != "load" ||
        instructions[1].operands() !=
            std::vector<std::string>{"int", instructions[0].result()} ||
        instructions[2].opcode() != "srem" ||
        instructions[2].operands() !=
            std::vector<std::string>{instructions[1].result(), "11"} ||
        instructions[3].opcode() != "store" ||
        instructions[3].operands() !=
            std::vector<std::string>{"int", instructions[2].result(),
                                     instructions[0].result()} ||
        instructions[4].opcode() != "add" ||
        instructions[4].operands().size() != 2 ||
        !((instructions[4].operands()[0] == induction &&
           instructions[4].operands()[1] == "1") ||
          (instructions[4].operands()[1] == induction &&
           instructions[4].operands()[0] == "1")) ||
        instructions[5].opcode() != "br" ||
        instructions[5].operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const auto &phiOperands = headerInstructions[0].operands();
    if (phiOperands.size() != 5 || phiOperands[0] != "int")
      continue;
    std::string preheaderName;
    for (std::size_t index = 1; index + 1 < phiOperands.size(); index += 2) {
      if (phiOperands[index + 1] != bodyName)
        preheaderName = phiOperands[index + 1];
    }
    if (preheaderName.empty())
      continue;
    const auto *preheader = blockByName(function, preheaderName);
    if (preheader == nullptr || preheader->terminator() == nullptr ||
        preheader->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    const auto initial = incomingValue(headerInstructions[0], preheaderName);
    const auto backedge = incomingValue(headerInstructions[0], bodyName);
    if (!initial.has_value() || !backedge.has_value() ||
        *backedge != instructions[4].result()) {
      continue;
    }

    return Candidate{header.name(),
                     bodyName,
                     preheaderName,
                     induction,
                     *backedge,
                     *initial,
                     headerInstructions[1].operands()[1],
                     instructions[0].operands()[0]};
  }
  return std::nullopt;
}

std::unordered_set<std::string> collectNames(const Function &function,
                                             bool blocks) {
  std::unordered_set<std::string> names;
  if (blocks) {
    for (const auto &block : function.blocks())
      names.insert(block.name());
    return names;
  }
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

std::string freshName(std::unordered_set<std::string> &names,
                      const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

Instruction rewriteInductionPhi(const Instruction &phi,
                                const Candidate &candidate,
                                const std::string &guard,
                                const std::string &finish,
                                const std::string &vectorEnd) {
  auto operands = phi.operands();
  for (std::size_t index = 2; index < operands.size(); index += 2) {
    if (operands[index] == candidate.preheader)
      operands[index] = guard;
  }
  operands.push_back(vectorEnd);
  operands.push_back(finish);
  return Instruction{"phi", std::move(operands), phi.result()};
}

bool transform(Function &function, const Candidate &candidate) {
  auto valueNames = collectNames(function, false);
  auto blockNames = collectNames(function, true);
  auto *preheader = blockByName(function, candidate.preheader);
  auto *header = blockByName(function, candidate.header);
  if (preheader == nullptr || header == nullptr ||
      preheader->terminator() == nullptr)
    return false;

  const std::string guardName =
      freshName(blockNames, "vector.remainder.guard");
  const std::string prepareName =
      freshName(blockNames, "vector.remainder.prepare");
  const std::string vectorHeaderName =
      freshName(blockNames, "vector.remainder.header");
  const std::string vectorBodyName =
      freshName(blockNames, "vector.remainder.body");
  const std::string finishName =
      freshName(blockNames, "vector.remainder.finish");
  const std::string nonnegative =
      freshName(valueNames, "%vector.remainder.nonnegative");
  const std::string ordered =
      freshName(valueNames, "%vector.remainder.ordered");
  const std::string remaining =
      freshName(valueNames, "%vector.remainder.remaining");
  const std::string enough =
      freshName(valueNames, "%vector.remainder.enough");
  const std::string safe =
      freshName(valueNames, "%vector.remainder.safe");
  const std::string canEnter =
      freshName(valueNames, "%vector.remainder.can.enter");
  const std::string trips =
      freshName(valueNames, "%vector.remainder.trips");
  const std::string vectorEnd =
      freshName(valueNames, "%vector.remainder.end");
  const std::string vectorIndex =
      freshName(valueNames, "%vector.remainder.index");
  const std::string vectorHasWork =
      freshName(valueNames, "%vector.remainder.has.work");
  const std::string nextIndex =
      freshName(valueNames, "%vector.remainder.next");

  preheader->instructions().back() = Instruction{"br", {guardName}};

  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"icmp.sge", {candidate.initialInduction, "0"}, nonnegative});
  guard.addInstruction(Instruction{
      "icmp.sge", {candidate.bound, candidate.initialInduction}, ordered});
  guard.addInstruction(Instruction{
      "sub", {candidate.bound, candidate.initialInduction}, remaining});
  guard.addInstruction(Instruction{
      "icmp.sge", {remaining, std::to_string(kVectorWidth)}, enough});
  guard.addInstruction(Instruction{"and", {nonnegative, ordered}, safe});
  guard.addInstruction(Instruction{"and", {safe, enough}, canEnter});
  guard.addInstruction(
      Instruction{"br", {canEnter, prepareName, candidate.header}});

  BasicBlock prepare{prepareName};
  prepare.addInstruction(Instruction{"and", {remaining, "-4"}, trips});
  prepare.addInstruction(
      Instruction{"add", {candidate.initialInduction, trips}, vectorEnd});
  prepare.addInstruction(
      Instruction{"aarch64.neon.srem.i32x4.init", {"11"}});
  prepare.addInstruction(Instruction{"br", {vectorHeaderName}});

  BasicBlock vectorHeader{vectorHeaderName};
  vectorHeader.addInstruction(
      Instruction{"phi",
                  {"int", candidate.initialInduction, prepareName, nextIndex,
                   vectorBodyName},
                  vectorIndex});
  vectorHeader.addInstruction(
      Instruction{"icmp.slt", {vectorIndex, vectorEnd}, vectorHasWork});
  vectorHeader.addInstruction(Instruction{
      "br", {vectorHasWork, vectorBodyName, finishName}});

  BasicBlock vectorBody{vectorBodyName};
  vectorBody.addInstruction(Instruction{"aarch64.neon.srem.i32x4.step",
                                        {candidate.base, vectorIndex}});
  vectorBody.addInstruction(Instruction{
      "add", {vectorIndex, std::to_string(kVectorWidth)}, nextIndex});
  vectorBody.addInstruction(Instruction{"br", {vectorHeaderName}});

  BasicBlock finish{finishName};
  finish.addInstruction(Instruction{"br", {candidate.header}});

  header->instructions()[0] =
      rewriteInductionPhi(header->instructions()[0], candidate, guardName,
                          finishName, vectorEnd);

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

void AArch64VectorConstantRemainderPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_VECTOR_CONSTANT_REMAINDER") !=
      nullptr) {
    return;
  }
  for (auto &function : module.functions()) {
    for (std::size_t count = 0; count < kMaximumTransformsPerFunction;
         ++count) {
      const auto candidate = matchCandidate(function);
      if (!candidate.has_value() || !transform(function, *candidate))
        break;
    }
  }
}

} // namespace compiler::midend::pass
