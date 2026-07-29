#include "../../../include/compiler/midend/pass/LoopInterchangeRowUpdatePass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
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

struct Candidate {
  std::string header;
  std::string outerBody;
  std::string innerHeader;
  std::string innerBody;
  std::string outerLatch;
  std::string exit;
  std::string rowBase;
  std::string matrixBase;
  std::string rowIndex;
  std::string bound;
  std::string entryPredecessor;
  std::string outerIndex;
};

bool isGep(const Instruction &instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "gep";
}

const Instruction *definitionOf(
    const std::unordered_map<std::string, const Instruction *> &definitions,
    const std::string &value) {
  const auto found = definitions.find(value);
  return found == definitions.end() ? nullptr : found->second;
}

const BasicBlock *
blockByName(const std::unordered_map<std::string, const BasicBlock *> &blocks,
            const std::string &name) {
  const auto found = blocks.find(name);
  return found == blocks.end() ? nullptr : found->second;
}

std::optional<std::string>
otherPhiIncomingValue(const Instruction &phi,
                      const std::string &knownPredecessor,
                      const std::string &knownValue) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5) {
    return std::nullopt;
  }
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index] == knownValue &&
        phi.operands()[index + 1] == knownPredecessor) {
      const std::size_t other = index == 1 ? 3 : 1;
      return phi.operands()[other];
    }
  }
  return std::nullopt;
}

bool phiHasIncoming(const Instruction &phi, const std::string &value,
                    const std::string &predecessor) {
  if (phi.opcode() != "phi" || phi.operands().size() < 3)
    return false;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index] == value &&
        phi.operands()[index + 1] == predecessor) {
      return true;
    }
  }
  return false;
}

bool isAddOne(const Instruction *instruction, const std::string &value) {
  return instruction != nullptr && instruction->opcode() == "add" &&
         instruction->operands().size() == 2 &&
         ((instruction->operands()[0] == value &&
           instruction->operands()[1] == "1") ||
          (instruction->operands()[1] == value &&
           instruction->operands()[0] == "1"));
}

bool isCanonicalNonnegativeInduction(
    const std::unordered_map<std::string, const Instruction *> &definitions,
    const std::string &value) {
  const auto *phi = definitionOf(definitions, value);
  if (phi == nullptr || phi->opcode() != "phi" || phi->operands().size() != 5) {
    return false;
  }
  for (std::size_t index = 1; index + 1 < phi->operands().size(); index += 2) {
    if (phi->operands()[index] != "0")
      continue;
    const std::size_t other = index == 1 ? 3 : 1;
    return isAddOne(definitionOf(definitions, phi->operands()[other]), value);
  }
  return false;
}

std::unordered_map<std::string, std::size_t>
predecessorCounts(const Function &function) {
  std::unordered_map<std::string, std::size_t> counts;
  for (const auto &block : function.blocks())
    counts.emplace(block.name(), 0);
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      const auto found = counts.find(operands[index]);
      if (found != counts.end())
        ++found->second;
    }
  }
  return counts;
}

bool blockHasPhi(const BasicBlock &block) {
  return !block.instructions().empty() &&
         block.instructions().front().opcode() == "phi";
}

bool functionHasFloatOrVectorState(const Function &function) {
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if ((!instruction.opcode().empty() &&
           instruction.opcode().front() == 'f') ||
          instruction.opcode().rfind("aarch64.neon.", 0) == 0 ||
          std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    "float") != instruction.operands().end()) {
        return true;
      }
    }
  }
  return false;
}

std::optional<Candidate> matchHeader(const Function &function,
                                     const BasicBlock &header) {
  const auto &headerInstructions = header.instructions();
  if (headerInstructions.size() != 3 ||
      headerInstructions[0].opcode() != "phi" ||
      headerInstructions[1].opcode() != "icmp.slt" ||
      headerInstructions[1].operands().size() != 2 ||
      headerInstructions[2].opcode() != "br" ||
      headerInstructions[2].operands().size() != 3 ||
      headerInstructions[2].operands()[0] != headerInstructions[1].result() ||
      headerInstructions[1].operands()[0] != headerInstructions[0].result()) {
    return std::nullopt;
  }

  std::unordered_map<std::string, const BasicBlock *> blocks;
  std::unordered_map<std::string, const Instruction *> definitions;
  for (const auto &block : function.blocks()) {
    blocks.emplace(block.name(), &block);
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        definitions.emplace(instruction.result(), &instruction);
    }
  }
  const auto predecessors = predecessorCounts(function);
  const std::string j = headerInstructions[0].result();
  const std::string bound = headerInstructions[1].operands()[1];
  const std::string outerBodyName = headerInstructions[2].operands()[1];
  const std::string exitName = headerInstructions[2].operands()[2];
  const auto *outerBody = blockByName(blocks, outerBodyName);
  const auto *exit = blockByName(blocks, exitName);
  if (outerBody == nullptr || exit == nullptr || blockHasPhi(*exit) ||
      predecessors.at(outerBodyName) != 1) {
    return std::nullopt;
  }

  const auto &outerInstructions = outerBody->instructions();
  if (outerInstructions.size() != 3 || !isGep(outerInstructions[0]) ||
      !outerInstructions[0].hasResult() ||
      outerInstructions[0].operands().size() != 2 ||
      outerInstructions[0].operands()[1] != j ||
      outerInstructions[1].opcode() != "load" ||
      outerInstructions[1].operands().size() != 2 ||
      outerInstructions[1].operands()[1] != outerInstructions[0].result() ||
      outerInstructions[2].opcode() != "br" ||
      outerInstructions[2].operands().size() != 1) {
    return std::nullopt;
  }
  const std::string rowBase = outerInstructions[0].operands()[0];
  const std::string destination = outerInstructions[0].result();
  const std::string initialAccumulator = outerInstructions[1].result();
  const std::string innerHeaderName = outerInstructions[2].operands()[0];
  const auto *innerHeader = blockByName(blocks, innerHeaderName);
  if (innerHeader == nullptr || predecessors.at(innerHeaderName) != 2) {
    return std::nullopt;
  }

  const auto &innerHeaderInstructions = innerHeader->instructions();
  if (innerHeaderInstructions.size() != 4 ||
      innerHeaderInstructions[0].opcode() != "phi" ||
      innerHeaderInstructions[1].opcode() != "phi" ||
      innerHeaderInstructions[2].opcode() != "icmp.slt" ||
      innerHeaderInstructions[2].operands().size() != 2 ||
      innerHeaderInstructions[3].opcode() != "br" ||
      innerHeaderInstructions[3].operands().size() != 3 ||
      innerHeaderInstructions[3].operands()[0] !=
          innerHeaderInstructions[2].result()) {
    return std::nullopt;
  }

  const Instruction *kPhi = nullptr;
  const Instruction *accumulatorPhi = nullptr;
  for (std::size_t index = 0; index < 2; ++index) {
    const auto &phi = innerHeaderInstructions[index];
    if (phiHasIncoming(phi, "0", outerBodyName)) {
      kPhi = &phi;
    }
    if (phiHasIncoming(phi, initialAccumulator, outerBodyName)) {
      accumulatorPhi = &phi;
    }
  }
  if (kPhi == nullptr || accumulatorPhi == nullptr || kPhi == accumulatorPhi ||
      innerHeaderInstructions[2].operands()[0] != kPhi->result()) {
    return std::nullopt;
  }
  const std::string rowIndex = innerHeaderInstructions[2].operands()[1];
  if (!isCanonicalNonnegativeInduction(definitions, rowIndex)) {
    return std::nullopt;
  }
  const std::string innerBodyName = innerHeaderInstructions[3].operands()[1];
  const std::string outerLatchName = innerHeaderInstructions[3].operands()[2];
  const auto *innerBody = blockByName(blocks, innerBodyName);
  const auto *outerLatch = blockByName(blocks, outerLatchName);
  if (innerBody == nullptr || outerLatch == nullptr ||
      predecessors.at(innerBodyName) != 1 ||
      predecessors.at(outerLatchName) != 1) {
    return std::nullopt;
  }

  const auto &latchInstructions = outerLatch->instructions();
  if (latchInstructions.size() != 3 ||
      latchInstructions[0].opcode() != "store" ||
      latchInstructions[0].operands().size() != 3 ||
      latchInstructions[0].operands()[0] != "int" ||
      latchInstructions[0].operands()[1] != accumulatorPhi->result() ||
      latchInstructions[0].operands()[2] != destination ||
      !isAddOne(&latchInstructions[1], j) ||
      latchInstructions[2].opcode() != "br" ||
      latchInstructions[2].operands().size() != 1 ||
      latchInstructions[2].operands()[0] != header.name() ||
      !phiHasIncoming(headerInstructions[0], latchInstructions[1].result(),
                      outerLatchName)) {
    return std::nullopt;
  }
  const auto rowIndexIncoming = otherPhiIncomingValue(
      headerInstructions[0], outerLatchName, latchInstructions[1].result());
  if (!rowIndexIncoming.has_value() || *rowIndexIncoming != rowIndex) {
    return std::nullopt;
  }
  const std::string entryPredecessor =
      headerInstructions[0].operands()[2] == outerLatchName
          ? headerInstructions[0].operands()[4]
          : headerInstructions[0].operands()[2];

  const auto &innerInstructions = innerBody->instructions();
  if (innerInstructions.size() != 9 || !isGep(innerInstructions[0]) ||
      innerInstructions[0].operands().size() != 2 ||
      innerInstructions[0].operands()[0] != rowBase ||
      innerInstructions[0].operands()[1] != kPhi->result() ||
      innerInstructions[1].opcode() != "load" ||
      innerInstructions[1].operands().size() != 2 ||
      innerInstructions[1].operands()[1] != innerInstructions[0].result() ||
      !isGep(innerInstructions[2]) ||
      innerInstructions[2].operands().size() != 2 ||
      innerInstructions[2].operands()[1] != kPhi->result() ||
      !isGep(innerInstructions[3]) ||
      innerInstructions[3].operands().size() != 2 ||
      innerInstructions[3].operands()[0] != innerInstructions[2].result() ||
      innerInstructions[3].operands()[1] != j ||
      innerInstructions[4].opcode() != "load" ||
      innerInstructions[4].operands().size() != 2 ||
      innerInstructions[4].operands()[1] != innerInstructions[3].result() ||
      innerInstructions[5].opcode() != "mul" ||
      innerInstructions[5].operands().size() != 2 ||
      innerInstructions[6].opcode() != "sub" ||
      innerInstructions[6].operands().size() != 2 ||
      innerInstructions[6].operands()[0] != accumulatorPhi->result() ||
      innerInstructions[6].operands()[1] != innerInstructions[5].result() ||
      !isAddOne(&innerInstructions[7], kPhi->result()) ||
      innerInstructions[8].opcode() != "br" ||
      innerInstructions[8].operands().size() != 1 ||
      innerInstructions[8].operands()[0] != innerHeaderName) {
    return std::nullopt;
  }
  const bool productMatches =
      (innerInstructions[5].operands()[0] == innerInstructions[1].result() &&
       innerInstructions[5].operands()[1] == innerInstructions[4].result()) ||
      (innerInstructions[5].operands()[1] == innerInstructions[1].result() &&
       innerInstructions[5].operands()[0] == innerInstructions[4].result());
  if (!productMatches ||
      !phiHasIncoming(*kPhi, innerInstructions[7].result(), innerBodyName) ||
      !phiHasIncoming(*accumulatorPhi, innerInstructions[6].result(),
                      innerBodyName)) {
    return std::nullopt;
  }

  const auto *rowDefinition = definitionOf(definitions, rowBase);
  if (rowDefinition == nullptr || !isGep(*rowDefinition) ||
      rowDefinition->operands().size() != 2 ||
      rowDefinition->operands()[1] != rowIndex ||
      innerInstructions[2].operands()[0] != rowDefinition->operands()[0]) {
    return std::nullopt;
  }
  const std::string matrixBase = rowDefinition->operands()[0];

  std::unordered_set<std::string> removed = {outerBodyName, innerHeaderName,
                                             innerBodyName, outerLatchName};
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      if (removed.find(operands[index]) != removed.end() &&
          block.name() != header.name() && block.name() != outerBodyName &&
          block.name() != innerHeaderName && block.name() != innerBodyName &&
          block.name() != outerLatchName) {
        return std::nullopt;
      }
    }
  }

  return Candidate{header.name(), outerBodyName,    innerHeaderName,
                   innerBodyName, outerLatchName,   exitName,
                   rowBase,       matrixBase,       rowIndex,
                   bound,         entryPredecessor, j};
}

std::unordered_set<std::string> valueNames(const Function &function) {
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

std::unordered_set<std::string> blockNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks())
    names.insert(block.name());
  return names;
}

std::string freshValue(std::unordered_set<std::string> &names,
                       const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "%" + stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

std::string freshBlock(std::unordered_set<std::string> &names,
                       const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

void applyVectorCandidate(Function &function, const Candidate &candidate) {
  auto values = valueNames(function);
  auto blocks = blockNames(function);
  const bool useK8CoefficientLaneMls =
      std::getenv("COMPILER_DISABLE_K8_COEFFICIENT_LANE_MLS") == nullptr;
  const std::string setupName = freshBlock(blocks, "row.vector.setup");
  const std::string prepareName = freshBlock(blocks, "row.vector.prepare");
  const std::string kTileHeaderName =
      freshBlock(blocks, "row.vector.k.tile.header");
  const std::string kTilePrepareName =
      freshBlock(blocks, "row.vector.k.tile.prepare");
  const std::string fullKTileSetupName =
      freshBlock(blocks, "row.vector.k.full.setup");
  const std::string jHeaderName = freshBlock(blocks, "row.vector.j.header");
  const std::string initializeName =
      freshBlock(blocks, "row.vector.initialize");
  const std::string fullKBodyName =
      freshBlock(blocks, "row.vector.k.full.body");
  const std::string kHeaderName = freshBlock(blocks, "row.vector.k.header");
  const std::string kBodyName = freshBlock(blocks, "row.vector.k.body");
  const std::string storeName = freshBlock(blocks, "row.vector.store");

  const std::string canEnter = freshValue(values, "row.vector.can.enter");
  const std::string remaining = freshValue(values, "row.vector.remaining");
  const std::string vectorColumns = freshValue(values, "row.vector.columns");
  const std::string vectorEnd = freshValue(values, "row.vector.end");
  const std::string hasVector = freshValue(values, "row.vector.has.vector");
  const std::string kTile = freshValue(values, "row.vector.k.tile");
  const std::string hasKTile = freshValue(values, "row.vector.has.k.tile");
  const std::string tentativeKEnd =
      freshValue(values, "row.vector.k.tentative.end");
  const std::string fullKTile =
      freshValue(values, "row.vector.k.full.tile");
  const std::string kEnd = freshValue(values, "row.vector.k.end");
  const std::string tileCoefficientAddress =
      freshValue(values, "row.vector.k.coefficient.address");
  const std::string sourceTileRow0 =
      freshValue(values, "row.vector.k.source.row0");
  const std::string sourceTileNext =
      freshValue(values, "row.vector.k.source.next");
  const std::string sourceTileRow1 =
      freshValue(values, "row.vector.k.source.row1");
  const std::string j = freshValue(values, "row.vector.j");
  const std::string hasVectorColumn =
      freshValue(values, "row.vector.has.column");
  const std::string destination = freshValue(values, "row.vector.destination");
  const std::string k = freshValue(values, "row.vector.k");
  const std::string hasK = freshValue(values, "row.vector.has.k");
  const std::string coefficientAddress =
      freshValue(values, "row.vector.coefficient.address");
  const std::string coefficient = freshValue(values, "row.vector.coefficient");
  const std::string sourceRow = freshValue(values, "row.vector.source.row");
  const std::string source = freshValue(values, "row.vector.source");
  const std::string fullSource0 =
      freshValue(values, "row.vector.k.full.source0");
  const std::string fullSource1 =
      freshValue(values, "row.vector.k.full.source1");
  const std::string kNext = freshValue(values, "row.vector.k.next");
  const std::string jNext = freshValue(values, "row.vector.j.next");

  auto header = std::find_if(function.blocks().begin(), function.blocks().end(),
                             [&](const BasicBlock &block) {
                               return block.name() == candidate.header;
                             });
  auto entry = std::find_if(function.blocks().begin(), function.blocks().end(),
                            [&](const BasicBlock &block) {
                              return block.name() == candidate.entryPredecessor;
                            });
  if (header == function.blocks().end() || entry == function.blocks().end() ||
      header->instructions().empty() ||
      header->instructions().front().opcode() != "phi") {
    return;
  }

  auto entryBranch = entry->instructions().back().operands();
  bool retargeted = false;
  for (auto &operand : entryBranch) {
    if (operand == candidate.header) {
      operand = setupName;
      retargeted = true;
    }
  }
  if (!retargeted)
    return;
  entry->instructions().back() = Instruction{"br", std::move(entryBranch)};

  const Instruction originalPhi = header->instructions().front();
  std::string backedgeValue;
  for (std::size_t index = 1; index + 1 < originalPhi.operands().size();
       index += 2) {
    if (originalPhi.operands()[index + 1] == candidate.outerLatch) {
      backedgeValue = originalPhi.operands()[index];
    }
  }
  if (backedgeValue.empty())
    return;
  header->instructions().front() =
      Instruction{"phi",
                  {"int", candidate.rowIndex, setupName, vectorEnd, prepareName,
                   vectorEnd, kTileHeaderName, backedgeValue,
                   candidate.outerLatch},
                  candidate.outerIndex};

  BasicBlock setup{setupName};
  setup.addInstruction(
      Instruction{"icmp.slt", {candidate.rowIndex, candidate.bound}, canEnter});
  setup.addInstruction(
      Instruction{"br", {canEnter, prepareName, candidate.header}});

  BasicBlock prepare{prepareName};
  prepare.addInstruction(
      Instruction{"sub", {candidate.bound, candidate.rowIndex}, remaining});
  prepare.addInstruction(Instruction{"and", {remaining, "-32"}, vectorColumns});
  prepare.addInstruction(
      Instruction{"add", {candidate.rowIndex, vectorColumns}, vectorEnd});
  prepare.addInstruction(
      Instruction{"icmp.sgt", {vectorColumns, "0"}, hasVector});
  prepare.addInstruction(
      Instruction{"br", {hasVector, kTileHeaderName, candidate.header}});

  BasicBlock kTileHeader{kTileHeaderName};
  kTileHeader.addInstruction(
      Instruction{"phi",
                  {"int", "0", prepareName, kEnd, jHeaderName},
                  kTile});
  kTileHeader.addInstruction(
      Instruction{"icmp.slt", {kTile, candidate.rowIndex}, hasKTile});
  kTileHeader.addInstruction(
      Instruction{"br", {hasKTile, kTilePrepareName, candidate.header}});

  BasicBlock kTilePrepare{kTilePrepareName};
  kTilePrepare.addInstruction(
      Instruction{"add", {kTile, "8"}, tentativeKEnd});
  kTilePrepare.addInstruction(
      Instruction{useK8CoefficientLaneMls ? "icmp.sle" : "icmp.slt",
                  {tentativeKEnd, candidate.rowIndex},
                  fullKTile});
  kTilePrepare.addInstruction(
      Instruction{"select", {fullKTile, tentativeKEnd, candidate.rowIndex},
                  kEnd});
  if (useK8CoefficientLaneMls) {
    kTilePrepare.addInstruction(Instruction{
        "getelementptr", {candidate.rowBase, kTile}, tileCoefficientAddress});
    kTilePrepare.addInstruction(Instruction{
        "getelementptr", {candidate.matrixBase, kTile}, sourceTileRow0});
    kTilePrepare.addInstruction(
        Instruction{"add", {kTile, "1"}, sourceTileNext});
    kTilePrepare.addInstruction(Instruction{
        "getelementptr", {candidate.matrixBase, sourceTileNext},
        sourceTileRow1});
    kTilePrepare.addInstruction(
        Instruction{"br", {fullKTile, fullKTileSetupName, jHeaderName}});
  } else {
    kTilePrepare.addInstruction(Instruction{"br", {jHeaderName}});
  }

  BasicBlock fullKTileSetup{fullKTileSetupName};
  fullKTileSetup.addInstruction(Instruction{
      "aarch64.neon.load.coeff.i32x8", {tileCoefficientAddress}});
  fullKTileSetup.addInstruction(Instruction{"br", {jHeaderName}});

  BasicBlock jHeader{jHeaderName};
  if (useK8CoefficientLaneMls) {
    jHeader.addInstruction(Instruction{
        "phi",
        {"int", candidate.rowIndex, kTilePrepareName, candidate.rowIndex,
         fullKTileSetupName, jNext, storeName},
        j});
  } else {
    jHeader.addInstruction(Instruction{
        "phi", {"int", candidate.rowIndex, kTilePrepareName, jNext, storeName},
        j});
  }
  jHeader.addInstruction(
      Instruction{"icmp.slt", {j, vectorEnd}, hasVectorColumn});
  jHeader.addInstruction(
      Instruction{"br", {hasVectorColumn, initializeName, kTileHeaderName}});

  BasicBlock initialize{initializeName};
  initialize.addInstruction(
      Instruction{"getelementptr", {candidate.rowBase, j}, destination});
  initialize.addInstruction(
      Instruction{"aarch64.neon.load.acc.i32x32", {destination}});
  initialize.addInstruction(
      useK8CoefficientLaneMls
          ? Instruction{"br", {fullKTile, fullKBodyName, kHeaderName}}
          : Instruction{"br", {kHeaderName}});

  BasicBlock fullKBody{fullKBodyName};
  fullKBody.addInstruction(
      Instruction{"getelementptr", {sourceTileRow0, j}, fullSource0});
  fullKBody.addInstruction(
      Instruction{"getelementptr", {sourceTileRow1, j}, fullSource1});
  fullKBody.addInstruction(Instruction{
      "aarch64.neon.mls.acc.i32x32.k8.by.element",
      {fullSource0, fullSource1}});
  fullKBody.addInstruction(Instruction{"br", {storeName}});

  BasicBlock kHeader{kHeaderName};
  kHeader.addInstruction(
      Instruction{"phi", {"int", kTile, initializeName, kNext, kBodyName}, k});
  kHeader.addInstruction(
      Instruction{"icmp.slt", {k, kEnd}, hasK});
  kHeader.addInstruction(Instruction{"br", {hasK, kBodyName, storeName}});

  BasicBlock kBody{kBodyName};
  kBody.addInstruction(
      Instruction{"getelementptr", {candidate.rowBase, k}, coefficientAddress});
  kBody.addInstruction(
      Instruction{"load", {"int", coefficientAddress}, coefficient});
  kBody.addInstruction(
      Instruction{"getelementptr", {candidate.matrixBase, k}, sourceRow});
  kBody.addInstruction(Instruction{"getelementptr", {sourceRow, j}, source});
  kBody.addInstruction(
      Instruction{"aarch64.neon.mls.acc.i32x32", {source, coefficient}});
  kBody.addInstruction(Instruction{"add", {k, "1"}, kNext});
  kBody.addInstruction(Instruction{"br", {kHeaderName}});

  BasicBlock store{storeName};
  store.addInstruction(
      Instruction{"aarch64.neon.store.acc.i32x32", {destination}});
  store.addInstruction(Instruction{"add", {j, "32"}, jNext});
  store.addInstruction(Instruction{"br", {jHeaderName}});

  auto insertion = header;
  insertion = function.blocks().insert(insertion, std::move(setup));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(prepare));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(kTileHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(kTilePrepare));
  ++insertion;
  if (useK8CoefficientLaneMls) {
    insertion =
        function.blocks().insert(insertion, std::move(fullKTileSetup));
    ++insertion;
  }
  insertion = function.blocks().insert(insertion, std::move(jHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(initialize));
  ++insertion;
  if (useK8CoefficientLaneMls) {
    insertion = function.blocks().insert(insertion, std::move(fullKBody));
    ++insertion;
  }
  insertion = function.blocks().insert(insertion, std::move(kHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(kBody));
  ++insertion;
  function.blocks().insert(insertion, std::move(store));
}

} // namespace

void LoopInterchangeRowUpdatePass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_LOOP_INTERCHANGE_ROW_UPDATE") != nullptr) {
    return;
  }
  for (auto &function : module.functions()) {
    if (functionHasFloatOrVectorState(function))
      continue;
    std::optional<Candidate> candidate;
    for (const auto &block : function.blocks()) {
      candidate = matchHeader(function, block);
      if (candidate.has_value())
        break;
    }
    if (candidate.has_value())
      applyVectorCandidate(function, *candidate);
  }
}

} // namespace compiler::midend::pass
