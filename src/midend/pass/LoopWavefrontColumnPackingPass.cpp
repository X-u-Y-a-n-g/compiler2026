#include "../../../include/compiler/midend/pass/LoopWavefrontColumnPackingPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <regex>
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

struct Candidate {
  std::vector<std::string> removedBlocks;
  std::string entryPredecessor;
  std::string exit;
  std::string matrixBase;
  std::string rowIndex;
  std::string bound;
  std::string rowBase;
  std::size_t rowCapacity = 0;
};

bool isGep(const Instruction &instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "gep";
}

const BasicBlock *
findBlock(const std::unordered_map<std::string, const BasicBlock *> &blocks,
          const std::string &name) {
  const auto found = blocks.find(name);
  return found == blocks.end() ? nullptr : found->second;
}

bool isAddOne(const Instruction *instruction, const std::string &value) {
  return instruction != nullptr && instruction->opcode() == "add" &&
         instruction->operands().size() == 2 &&
         ((instruction->operands()[0] == value &&
           instruction->operands()[1] == "1") ||
          (instruction->operands()[1] == value &&
           instruction->operands()[0] == "1"));
}

bool phiHasIncoming(const Instruction &phi, const std::string &value,
                    const std::string &predecessor) {
  if (phi.opcode() != "phi")
    return false;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index] == value &&
        phi.operands()[index + 1] == predecessor) {
      return true;
    }
  }
  return false;
}

std::optional<std::string>
otherIncomingValue(const Instruction &phi, const std::string &knownValue,
                   const std::string &knownPredecessor) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5)
    return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index] == knownValue &&
        phi.operands()[index + 1] == knownPredecessor) {
      const std::size_t other = index == 1 ? 3 : 1;
      return phi.operands()[other];
    }
  }
  return std::nullopt;
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

std::optional<std::size_t> matrixRowCapacity(const Function &function,
                                             const std::string &base) {
  for (const auto &parameter : function.parameters()) {
    if (parameter.name != base)
      continue;
    // Array parameters preserve all declared extents in their IR type.  The
    // final positive extent is the number of contiguous scalar elements in a
    // row and is therefore a valid stack-buffer capacity.
    static const std::regex rankTwoInt{R"(^int\[\]\[([1-9][0-9]*)\]$)"};
    std::smatch match;
    if (!std::regex_match(parameter.type, match, rankTwoInt))
      return std::nullopt;
    try {
      const auto capacity = std::stoull(match[1].str());
      constexpr std::size_t kMaximumPackedBytes = 64U * 1024U;
      constexpr std::size_t kI32Bytes = 4;
      if (capacity == 0 || capacity > kMaximumPackedBytes / kI32Bytes)
        return std::nullopt;
      return static_cast<std::size_t>(capacity);
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool exactBranch(const BasicBlock &block,
                 const std::vector<std::string> &operands) {
  const auto *terminator = block.terminator();
  return terminator != nullptr && terminator->opcode() == "br" &&
         terminator->operands() == operands;
}

std::optional<Candidate> matchCandidate(const Function &function,
                                        const BasicBlock &outerHeader) {
  std::unordered_map<std::string, const BasicBlock *> blocks;
  for (const auto &block : function.blocks()) {
    blocks.emplace(block.name(), &block);
    for (const auto &instruction : block.instructions()) {
      const auto &opcode = instruction.opcode();
      if ((!opcode.empty() && opcode.front() == 'f') || opcode == "call" ||
          opcode.rfind("aarch64.neon.", 0) == 0) {
        return std::nullopt;
      }
    }
  }
  const auto predecessors = predecessorsOf(function);
  const auto &outer = outerHeader.instructions();
  if (outer.size() != 3 || outer[0].opcode() != "phi" ||
      outer[0].operands().size() != 5 || outer[1].opcode() != "icmp.slt" ||
      outer[1].operands().size() != 2 ||
      outer[1].operands()[0] != outer[0].result() ||
      outer[2].opcode() != "br" || outer[2].operands().size() != 3 ||
      outer[2].operands()[0] != outer[1].result()) {
    return std::nullopt;
  }
  const std::string row = outer[0].result();
  const std::string bound = outer[1].operands()[1];
  const std::string outerBodyName = outer[2].operands()[1];
  const std::string exitName = outer[2].operands()[2];
  const auto *outerBody = findBlock(blocks, outerBodyName);
  const auto *exit = findBlock(blocks, exitName);
  if (outerBody == nullptr || exit == nullptr ||
      (!exit->instructions().empty() &&
       exit->instructions().front().opcode() == "phi") ||
      predecessors.at(outerBodyName).size() != 1) {
    return std::nullopt;
  }
  const auto &outerBodyCode = outerBody->instructions();
  if (outerBodyCode.size() != 2 || !isGep(outerBodyCode[0]) ||
      outerBodyCode[0].operands().size() != 2 ||
      outerBodyCode[0].operands()[1] != row ||
      outerBodyCode[1].opcode() != "br" ||
      outerBodyCode[1].operands().size() != 1) {
    return std::nullopt;
  }
  const std::string matrix = outerBodyCode[0].operands()[0];
  const std::string rowBase = outerBodyCode[0].result();
  const auto capacity = matrixRowCapacity(function, matrix);
  if (!capacity.has_value()) {
    return std::nullopt;
  }

  const std::string lowerHeaderName = outerBodyCode[1].operands()[0];
  const auto *lowerHeader = findBlock(blocks, lowerHeaderName);
  if (lowerHeader == nullptr || lowerHeader->instructions().size() != 3)
    return std::nullopt;
  const auto &lh = lowerHeader->instructions();
  if (lh[0].opcode() != "phi" || lh[1].opcode() != "icmp.slt" ||
      lh[1].operands().size() != 2 || lh[1].operands()[0] != lh[0].result() ||
      lh[1].operands()[1] != row || lh[2].opcode() != "br" ||
      lh[2].operands().size() != 3 || lh[2].operands()[0] != lh[1].result()) {
    return std::nullopt;
  }
  const std::string lowerJ = lh[0].result();
  const std::string lowerBodyName = lh[2].operands()[1];
  const std::string lowerExitName = lh[2].operands()[2];
  const auto *lowerBody = findBlock(blocks, lowerBodyName);
  const auto *lowerExit = findBlock(blocks, lowerExitName);
  if (lowerBody == nullptr || lowerExit == nullptr ||
      lowerBody->instructions().size() != 4) {
    return std::nullopt;
  }
  std::string upperHeaderName = lowerExitName;
  bool hasLowerExitBridge = false;
  if (lowerExit->instructions().size() == 1 &&
      lowerExit->instructions()[0].opcode() == "br" &&
      lowerExit->instructions()[0].operands().size() == 1) {
    upperHeaderName = lowerExit->instructions()[0].operands()[0];
    hasLowerExitBridge = true;
  }
  const auto *upperHeader = findBlock(blocks, upperHeaderName);
  if (upperHeader == nullptr)
    return std::nullopt;
  const auto &lb = lowerBody->instructions();
  if (!isGep(lb[0]) ||
      lb[0].operands() != std::vector<std::string>{rowBase, lowerJ} ||
      lb[1].opcode() != "load" ||
      lb[1].operands() != std::vector<std::string>{"int", lb[0].result()} ||
      lb[2].opcode() != "sub" ||
      lb[2].operands() != std::vector<std::string>{lowerJ, "1"} ||
      lb[3].opcode() != "br" || lb[3].operands().size() != 1) {
    return std::nullopt;
  }
  const std::string lowerDestination = lb[0].result();
  const std::string lowerInitial = lb[1].result();
  const std::string previousColumn = lb[2].result();
  const std::string lowerKHeaderName = lb[3].operands()[0];
  const auto *lowerKHeader = findBlock(blocks, lowerKHeaderName);
  if (lowerKHeader == nullptr || lowerKHeader->instructions().size() != 4)
    return std::nullopt;
  const auto &lkh = lowerKHeader->instructions();
  const Instruction *lowerKPhi = nullptr;
  const Instruction *lowerAccPhi = nullptr;
  for (std::size_t index = 0; index < 2; ++index) {
    if (phiHasIncoming(lkh[index], "0", lowerBodyName))
      lowerKPhi = &lkh[index];
    if (phiHasIncoming(lkh[index], lowerInitial, lowerBodyName))
      lowerAccPhi = &lkh[index];
  }
  if (lowerKPhi == nullptr || lowerAccPhi == nullptr ||
      lkh[2].opcode() != "icmp.slt" ||
      lkh[2].operands() !=
          std::vector<std::string>{lowerKPhi->result(), lowerJ} ||
      lkh[3].opcode() != "br" || lkh[3].operands().size() != 3 ||
      lkh[3].operands()[0] != lkh[2].result()) {
    return std::nullopt;
  }
  const std::string lowerKBodyName = lkh[3].operands()[1];
  const std::string lowerLatchName = lkh[3].operands()[2];
  const auto *lowerKBody = findBlock(blocks, lowerKBodyName);
  const auto *lowerLatch = findBlock(blocks, lowerLatchName);
  if (lowerKBody == nullptr || lowerLatch == nullptr ||
      lowerKBody->instructions().size() != 9 ||
      lowerLatch->instructions().size() != 7) {
    return std::nullopt;
  }
  const auto &lkb = lowerKBody->instructions();
  if (!isGep(lkb[0]) ||
      lkb[0].operands() !=
          std::vector<std::string>{rowBase, lowerKPhi->result()} ||
      lkb[1].opcode() != "load" || lkb[1].operands()[1] != lkb[0].result() ||
      !isGep(lkb[2]) ||
      lkb[2].operands() !=
          std::vector<std::string>{matrix, lowerKPhi->result()} ||
      !isGep(lkb[3]) ||
      lkb[3].operands() !=
          std::vector<std::string>{lkb[2].result(), previousColumn} ||
      lkb[4].opcode() != "load" || lkb[4].operands()[1] != lkb[3].result() ||
      lkb[5].opcode() != "mul" ||
      !((lkb[5].operands()[0] == lkb[1].result() &&
         lkb[5].operands()[1] == lkb[4].result()) ||
        (lkb[5].operands()[1] == lkb[1].result() &&
         lkb[5].operands()[0] == lkb[4].result())) ||
      lkb[6].opcode() != "sub" ||
      lkb[6].operands() !=
          std::vector<std::string>{lowerAccPhi->result(), lkb[5].result()} ||
      !isAddOne(&lkb[7], lowerKPhi->result()) ||
      !exactBranch(*lowerKBody, {lowerKHeaderName}) ||
      !phiHasIncoming(*lowerKPhi, lkb[7].result(), lowerKBodyName) ||
      !phiHasIncoming(*lowerAccPhi, lkb[6].result(), lowerKBodyName)) {
    return std::nullopt;
  }
  const auto &ll = lowerLatch->instructions();
  if (!isGep(ll[0]) ||
      ll[0].operands() != std::vector<std::string>{matrix, lowerJ} ||
      !isGep(ll[1]) ||
      ll[1].operands() != std::vector<std::string>{ll[0].result(), lowerJ} ||
      ll[2].opcode() != "load" || ll[2].operands()[1] != ll[1].result() ||
      ll[3].opcode() != "sdiv" ||
      ll[3].operands() !=
          std::vector<std::string>{lowerAccPhi->result(), ll[2].result()} ||
      ll[4].opcode() != "store" ||
      ll[4].operands() !=
          std::vector<std::string>{"int", ll[3].result(), lowerDestination} ||
      !isAddOne(&ll[5], lowerJ) ||
      !exactBranch(*lowerLatch, {lowerHeaderName}) ||
      !phiHasIncoming(lh[0], "0", outerBodyName) ||
      !phiHasIncoming(lh[0], ll[5].result(), lowerLatchName)) {
    return std::nullopt;
  }

  const auto &uh = upperHeader->instructions();
  if (uh.size() != 3 || uh[0].opcode() != "phi" ||
      uh[1].opcode() != "icmp.slt" || uh[1].operands().size() != 2 ||
      uh[1].operands()[0] != uh[0].result() || uh[1].operands()[1] != bound ||
      uh[2].opcode() != "br" || uh[2].operands().size() != 3 ||
      uh[2].operands()[0] != uh[1].result()) {
    return std::nullopt;
  }
  const std::string upperJ = uh[0].result();
  const std::string upperBodyName = uh[2].operands()[1];
  const std::string outerLatchName = uh[2].operands()[2];
  const auto *upperBody = findBlock(blocks, upperBodyName);
  const auto *outerLatch = findBlock(blocks, outerLatchName);
  if (upperBody == nullptr || outerLatch == nullptr ||
      upperBody->instructions().size() != 3 ||
      outerLatch->instructions().size() != 2) {
    return std::nullopt;
  }
  const auto &ub = upperBody->instructions();
  if (!isGep(ub[0]) ||
      ub[0].operands() != std::vector<std::string>{rowBase, upperJ} ||
      ub[1].opcode() != "load" || ub[1].operands()[1] != ub[0].result() ||
      ub[2].opcode() != "br" || ub[2].operands().size() != 1) {
    return std::nullopt;
  }
  const std::string upperDestination = ub[0].result();
  const std::string upperInitial = ub[1].result();
  const std::string upperKHeaderName = ub[2].operands()[0];
  const auto *upperKHeader = findBlock(blocks, upperKHeaderName);
  if (upperKHeader == nullptr || upperKHeader->instructions().size() != 4)
    return std::nullopt;
  const auto &ukh = upperKHeader->instructions();
  const Instruction *upperKPhi = nullptr;
  const Instruction *upperAccPhi = nullptr;
  for (std::size_t index = 0; index < 2; ++index) {
    if (phiHasIncoming(ukh[index], "0", upperBodyName))
      upperKPhi = &ukh[index];
    if (phiHasIncoming(ukh[index], upperInitial, upperBodyName))
      upperAccPhi = &ukh[index];
  }
  if (upperKPhi == nullptr || upperAccPhi == nullptr ||
      ukh[2].opcode() != "icmp.slt" ||
      ukh[2].operands() != std::vector<std::string>{upperKPhi->result(), row} ||
      ukh[3].opcode() != "br" || ukh[3].operands().size() != 3 ||
      ukh[3].operands()[0] != ukh[2].result()) {
    return std::nullopt;
  }
  const std::string upperKBodyName = ukh[3].operands()[1];
  const std::string upperLatchName = ukh[3].operands()[2];
  const auto *upperKBody = findBlock(blocks, upperKBodyName);
  const auto *upperLatch = findBlock(blocks, upperLatchName);
  if (upperKBody == nullptr || upperLatch == nullptr ||
      upperKBody->instructions().size() != 9 ||
      upperLatch->instructions().size() != 3) {
    return std::nullopt;
  }
  const auto &ukb = upperKBody->instructions();
  if (!isGep(ukb[0]) ||
      ukb[0].operands() !=
          std::vector<std::string>{rowBase, upperKPhi->result()} ||
      ukb[1].opcode() != "load" || ukb[1].operands()[1] != ukb[0].result() ||
      !isGep(ukb[2]) ||
      ukb[2].operands() !=
          std::vector<std::string>{matrix, upperKPhi->result()} ||
      !isGep(ukb[3]) ||
      ukb[3].operands() != std::vector<std::string>{ukb[2].result(), upperJ} ||
      ukb[4].opcode() != "load" || ukb[4].operands()[1] != ukb[3].result() ||
      ukb[5].opcode() != "mul" ||
      !((ukb[5].operands()[0] == ukb[1].result() &&
         ukb[5].operands()[1] == ukb[4].result()) ||
        (ukb[5].operands()[1] == ukb[1].result() &&
         ukb[5].operands()[0] == ukb[4].result())) ||
      ukb[6].opcode() != "sub" ||
      ukb[6].operands() !=
          std::vector<std::string>{upperAccPhi->result(), ukb[5].result()} ||
      !isAddOne(&ukb[7], upperKPhi->result()) ||
      !exactBranch(*upperKBody, {upperKHeaderName}) ||
      !phiHasIncoming(*upperKPhi, ukb[7].result(), upperKBodyName) ||
      !phiHasIncoming(*upperAccPhi, ukb[6].result(), upperKBodyName)) {
    return std::nullopt;
  }
  const auto &ul = upperLatch->instructions();
  if (ul[0].opcode() != "store" ||
      ul[0].operands() != std::vector<std::string>{"int", upperAccPhi->result(),
                                                   upperDestination} ||
      !isAddOne(&ul[1], upperJ) ||
      !exactBranch(*upperLatch, {upperHeaderName}) ||
      !phiHasIncoming(uh[0], row,
                      hasLowerExitBridge ? lowerExitName : lowerHeaderName) ||
      !phiHasIncoming(uh[0], ul[1].result(), upperLatchName)) {
    return std::nullopt;
  }
  const auto &ol = outerLatch->instructions();
  if (!isAddOne(&ol[0], row) ||
      !exactBranch(*outerLatch, {outerHeader.name()}) ||
      !phiHasIncoming(outer[0], "0",
                      outer[0].operands()[2] == outerLatchName
                          ? outer[0].operands()[4]
                          : outer[0].operands()[2]) ||
      !phiHasIncoming(outer[0], ol[0].result(), outerLatchName)) {
    return std::nullopt;
  }
  const auto entryValue =
      otherIncomingValue(outer[0], ol[0].result(), outerLatchName);
  if (!entryValue.has_value() || *entryValue != "0")
    return std::nullopt;
  const std::string entryPredecessor = outer[0].operands()[2] == outerLatchName
                                           ? outer[0].operands()[4]
                                           : outer[0].operands()[2];

  std::vector<std::string> removed{
      outerHeader.name(), outerBodyName,    lowerHeaderName, lowerBodyName,
      lowerKHeaderName,   lowerKBodyName,   lowerLatchName,  upperHeaderName,
      upperBodyName,      upperKHeaderName, upperKBodyName,  upperLatchName,
      outerLatchName};
  if (hasLowerExitBridge)
    removed.push_back(lowerExitName);
  const std::unordered_set<std::string> removedSet(removed.begin(),
                                                   removed.end());
  for (const auto &block : function.blocks()) {
    const auto *terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      if (removedSet.find(operands[index]) != removedSet.end() &&
          removedSet.find(block.name()) == removedSet.end() &&
          block.name() != entryPredecessor) {
        return std::nullopt;
      }
    }
  }

  return Candidate{std::move(removed),
                   entryPredecessor,
                   exitName,
                   matrix,
                   row,
                   bound,
                   rowBase,
                   *capacity};
}

std::unordered_set<std::string> valueNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &parameter : function.parameters())
    names.insert(parameter.name);
  for (const auto &block : function.blocks())
    for (const auto &instruction : block.instructions())
      if (instruction.hasResult())
        names.insert(instruction.result());
  return names;
}

std::unordered_set<std::string> blockNames(const Function &function) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks())
    names.insert(block.name());
  return names;
}

std::string fresh(std::unordered_set<std::string> &names,
                  const std::string &stem, bool value) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate =
        (value ? "%" : "") + stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second)
      return candidate;
  }
}

bool applyCandidate(Function &function, const Candidate &candidate) {
  auto values = valueNames(function);
  auto blocks = blockNames(function);
  auto entry = std::find_if(function.blocks().begin(), function.blocks().end(),
                            [&](const BasicBlock &block) {
                              return block.name() == candidate.entryPredecessor;
                            });
  if (entry == function.blocks().end() || entry->terminator() == nullptr)
    return false;

  const std::string waveHeaderName = fresh(blocks, "wave.header", false);
  const std::string upperSetupName = fresh(blocks, "wave.upper.setup", false);
  const std::string upperHeaderName = fresh(blocks, "wave.upper.header", false);
  const std::string upperBodyName = fresh(blocks, "wave.upper.body", false);
  const std::string upperKHeaderName =
      fresh(blocks, "wave.upper.k.header", false);
  const std::string upperKBodyName = fresh(blocks, "wave.upper.k.body", false);
  const std::string upperLatchName = fresh(blocks, "wave.upper.latch", false);
  const std::string packSetupName = fresh(blocks, "wave.pack.setup", false);
  const std::string packHeaderName = fresh(blocks, "wave.pack.header", false);
  const std::string packBodyName = fresh(blocks, "wave.pack.body", false);
  const std::string lowerSetupName = fresh(blocks, "wave.lower.setup", false);
  const std::string lowerHeaderName = fresh(blocks, "wave.lower.header", false);
  const std::string lowerBodyName = fresh(blocks, "wave.lower.body", false);
  const std::string lowerKHeaderName =
      fresh(blocks, "wave.lower.k.header", false);
  const std::string lowerKBodyName = fresh(blocks, "wave.lower.k.body", false);
  const std::string lowerLatchName = fresh(blocks, "wave.lower.latch", false);
  const std::string waveLatchName = fresh(blocks, "wave.latch", false);

  const std::string packed = fresh(values, "wave.packed", true);
  const std::string hasRow = fresh(values, "wave.has.row", true);
  const std::string upperRowBase = fresh(values, "wave.upper.row", true);
  const std::string upperJ = fresh(values, "wave.upper.j", true);
  const std::string hasUpperJ = fresh(values, "wave.upper.has.j", true);
  const std::string upperDestination =
      fresh(values, "wave.upper.destination", true);
  const std::string upperInitial = fresh(values, "wave.upper.initial", true);
  const std::string upperK = fresh(values, "wave.upper.k", true);
  const std::string upperAcc = fresh(values, "wave.upper.acc", true);
  const std::string hasUpperK = fresh(values, "wave.upper.has.k", true);
  const std::string upperCoefficientAddress =
      fresh(values, "wave.upper.coeff.addr", true);
  const std::string upperCoefficient = fresh(values, "wave.upper.coeff", true);
  const std::string upperSourceRow =
      fresh(values, "wave.upper.source.row", true);
  const std::string upperSourceAddress =
      fresh(values, "wave.upper.source.addr", true);
  const std::string upperSource = fresh(values, "wave.upper.source", true);
  const std::string upperProduct = fresh(values, "wave.upper.product", true);
  const std::string upperAccNext = fresh(values, "wave.upper.acc.next", true);
  const std::string upperKNext = fresh(values, "wave.upper.k.next", true);
  const std::string upperJNext = fresh(values, "wave.upper.j.next", true);
  const std::string packedColumn = fresh(values, "wave.packed.column", true);
  const std::string packK = fresh(values, "wave.pack.k", true);
  const std::string hasPackK = fresh(values, "wave.pack.has.k", true);
  const std::string packSourceRow = fresh(values, "wave.pack.source.row", true);
  const std::string packSourceAddress =
      fresh(values, "wave.pack.source.addr", true);
  const std::string packValue = fresh(values, "wave.pack.value", true);
  const std::string packDestination =
      fresh(values, "wave.pack.destination", true);
  const std::string packKNext = fresh(values, "wave.pack.k.next", true);
  const std::string lowerStart = fresh(values, "wave.lower.start", true);
  const std::string lowerRow = fresh(values, "wave.lower.row.index", true);
  const std::string hasLowerRow = fresh(values, "wave.lower.has.row", true);
  const std::string lowerRowBase = fresh(values, "wave.lower.row", true);
  const std::string lowerDestination =
      fresh(values, "wave.lower.destination", true);
  const std::string lowerInitial = fresh(values, "wave.lower.initial", true);
  const std::string lowerK = fresh(values, "wave.lower.k", true);
  const std::string lowerAcc = fresh(values, "wave.lower.acc", true);
  const std::string hasLowerK = fresh(values, "wave.lower.has.k", true);
  const std::string lowerLeftAddress =
      fresh(values, "wave.lower.left.addr", true);
  const std::string lowerLeft = fresh(values, "wave.lower.left", true);
  const std::string lowerRightAddress =
      fresh(values, "wave.lower.right.addr", true);
  const std::string lowerRight = fresh(values, "wave.lower.right", true);
  const std::string lowerProduct = fresh(values, "wave.lower.product", true);
  const std::string lowerAccNext = fresh(values, "wave.lower.acc.next", true);
  const std::string lowerKNext = fresh(values, "wave.lower.k.next", true);
  const std::string diagonalRow = fresh(values, "wave.diagonal.row", true);
  const std::string diagonalAddress = fresh(values, "wave.diagonal.addr", true);
  const std::string diagonal = fresh(values, "wave.diagonal", true);
  const std::string quotient = fresh(values, "wave.quotient", true);
  const std::string lowerRowNext = fresh(values, "wave.lower.row.next", true);
  const std::string rowNext = fresh(values, "wave.row.next", true);

  auto branchOperands = entry->terminator()->operands();
  bool retargeted = false;
  for (auto &operand : branchOperands) {
    if (std::find(candidate.removedBlocks.begin(),
                  candidate.removedBlocks.end(),
                  operand) != candidate.removedBlocks.end()) {
      operand = waveHeaderName;
      retargeted = true;
    }
  }
  if (!retargeted)
    return false;
  entry->instructions().back() = Instruction{"br", std::move(branchOperands)};
  entry->instructions().insert(
      entry->instructions().end() - 1,
      Instruction{"alloca",
                  {"int[" + std::to_string(candidate.rowCapacity) + "]"},
                  packed});

  BasicBlock waveHeader{waveHeaderName};
  waveHeader.addInstruction(Instruction{
      "phi",
      {"int", "0", candidate.entryPredecessor, rowNext, waveLatchName},
      candidate.rowIndex});
  waveHeader.addInstruction(
      Instruction{"icmp.slt", {candidate.rowIndex, candidate.bound}, hasRow});
  waveHeader.addInstruction(
      Instruction{"br", {hasRow, upperSetupName, candidate.exit}});

  BasicBlock upperSetup{upperSetupName};
  upperSetup.addInstruction(
      Instruction{"getelementptr",
                  {candidate.matrixBase, candidate.rowIndex},
                  upperRowBase});
  upperSetup.addInstruction(Instruction{"br", {upperHeaderName}});

  BasicBlock upperHeader{upperHeaderName};
  upperHeader.addInstruction(Instruction{
      "phi",
      {"int", candidate.rowIndex, upperSetupName, upperJNext, upperLatchName},
      upperJ});
  upperHeader.addInstruction(
      Instruction{"icmp.slt", {upperJ, candidate.bound}, hasUpperJ});
  upperHeader.addInstruction(
      Instruction{"br", {hasUpperJ, upperBodyName, packSetupName}});

  BasicBlock upperBody{upperBodyName};
  upperBody.addInstruction(
      Instruction{"getelementptr", {upperRowBase, upperJ}, upperDestination});
  upperBody.addInstruction(
      Instruction{"load", {"int", upperDestination}, upperInitial});
  upperBody.addInstruction(Instruction{"br", {upperKHeaderName}});

  BasicBlock upperKHeader{upperKHeaderName};
  upperKHeader.addInstruction(Instruction{
      "phi", {"int", "0", upperBodyName, upperKNext, upperKBodyName}, upperK});
  upperKHeader.addInstruction(Instruction{
      "phi",
      {"int", upperInitial, upperBodyName, upperAccNext, upperKBodyName},
      upperAcc});
  upperKHeader.addInstruction(
      Instruction{"icmp.slt", {upperK, candidate.rowIndex}, hasUpperK});
  upperKHeader.addInstruction(
      Instruction{"br", {hasUpperK, upperKBodyName, upperLatchName}});

  BasicBlock upperKBody{upperKBodyName};
  upperKBody.addInstruction(Instruction{
      "getelementptr", {upperRowBase, upperK}, upperCoefficientAddress});
  upperKBody.addInstruction(
      Instruction{"load", {"int", upperCoefficientAddress}, upperCoefficient});
  upperKBody.addInstruction(Instruction{
      "getelementptr", {candidate.matrixBase, upperK}, upperSourceRow});
  upperKBody.addInstruction(Instruction{
      "getelementptr", {upperSourceRow, upperJ}, upperSourceAddress});
  upperKBody.addInstruction(
      Instruction{"load", {"int", upperSourceAddress}, upperSource});
  upperKBody.addInstruction(
      Instruction{"mul", {upperCoefficient, upperSource}, upperProduct});
  upperKBody.addInstruction(
      Instruction{"sub", {upperAcc, upperProduct}, upperAccNext});
  upperKBody.addInstruction(Instruction{"add", {upperK, "1"}, upperKNext});
  upperKBody.addInstruction(Instruction{"br", {upperKHeaderName}});

  BasicBlock upperLatch{upperLatchName};
  upperLatch.addInstruction(
      Instruction{"store", {"int", upperAcc, upperDestination}});
  upperLatch.addInstruction(Instruction{"add", {upperJ, "1"}, upperJNext});
  upperLatch.addInstruction(Instruction{"br", {upperHeaderName}});

  BasicBlock packSetup{packSetupName};
  packSetup.addInstruction(
      Instruction{"sub", {candidate.rowIndex, "1"}, packedColumn});
  packSetup.addInstruction(Instruction{"br", {packHeaderName}});

  BasicBlock packHeader{packHeaderName};
  packHeader.addInstruction(Instruction{
      "phi", {"int", "0", packSetupName, packKNext, packBodyName}, packK});
  packHeader.addInstruction(
      Instruction{"icmp.slt", {packK, candidate.rowIndex}, hasPackK});
  packHeader.addInstruction(
      Instruction{"br", {hasPackK, packBodyName, lowerSetupName}});

  BasicBlock packBody{packBodyName};
  packBody.addInstruction(Instruction{
      "getelementptr", {candidate.matrixBase, packK}, packSourceRow});
  packBody.addInstruction(Instruction{
      "getelementptr", {packSourceRow, packedColumn}, packSourceAddress});
  packBody.addInstruction(
      Instruction{"load", {"int", packSourceAddress}, packValue});
  packBody.addInstruction(
      Instruction{"getelementptr", {packed, packK}, packDestination});
  packBody.addInstruction(
      Instruction{"store", {"int", packValue, packDestination}});
  packBody.addInstruction(Instruction{"add", {packK, "1"}, packKNext});
  packBody.addInstruction(Instruction{"br", {packHeaderName}});

  BasicBlock lowerSetup{lowerSetupName};
  lowerSetup.addInstruction(
      Instruction{"add", {candidate.rowIndex, "1"}, lowerStart});
  lowerSetup.addInstruction(Instruction{"br", {lowerHeaderName}});

  BasicBlock lowerHeader{lowerHeaderName};
  lowerHeader.addInstruction(Instruction{
      "phi",
      {"int", lowerStart, lowerSetupName, lowerRowNext, lowerLatchName},
      lowerRow});
  lowerHeader.addInstruction(
      Instruction{"icmp.slt", {lowerRow, candidate.bound}, hasLowerRow});
  lowerHeader.addInstruction(
      Instruction{"br", {hasLowerRow, lowerBodyName, waveLatchName}});

  BasicBlock lowerBody{lowerBodyName};
  lowerBody.addInstruction(Instruction{
      "getelementptr", {candidate.matrixBase, lowerRow}, lowerRowBase});
  lowerBody.addInstruction(Instruction{
      "getelementptr", {lowerRowBase, candidate.rowIndex}, lowerDestination});
  lowerBody.addInstruction(
      Instruction{"load", {"int", lowerDestination}, lowerInitial});
  lowerBody.addInstruction(Instruction{"br", {lowerKHeaderName}});

  BasicBlock lowerKHeader{lowerKHeaderName};
  lowerKHeader.addInstruction(Instruction{
      "phi", {"int", "0", lowerBodyName, lowerKNext, lowerKBodyName}, lowerK});
  lowerKHeader.addInstruction(Instruction{
      "phi",
      {"int", lowerInitial, lowerBodyName, lowerAccNext, lowerKBodyName},
      lowerAcc});
  lowerKHeader.addInstruction(
      Instruction{"icmp.slt", {lowerK, candidate.rowIndex}, hasLowerK});
  lowerKHeader.addInstruction(
      Instruction{"br", {hasLowerK, lowerKBodyName, lowerLatchName}});

  BasicBlock lowerKBody{lowerKBodyName};
  lowerKBody.addInstruction(
      Instruction{"getelementptr", {lowerRowBase, lowerK}, lowerLeftAddress});
  lowerKBody.addInstruction(
      Instruction{"load", {"int", lowerLeftAddress}, lowerLeft});
  lowerKBody.addInstruction(
      Instruction{"getelementptr", {packed, lowerK}, lowerRightAddress});
  lowerKBody.addInstruction(
      Instruction{"load", {"int", lowerRightAddress}, lowerRight});
  lowerKBody.addInstruction(
      Instruction{"mul", {lowerLeft, lowerRight}, lowerProduct});
  lowerKBody.addInstruction(
      Instruction{"sub", {lowerAcc, lowerProduct}, lowerAccNext});
  lowerKBody.addInstruction(Instruction{"add", {lowerK, "1"}, lowerKNext});
  lowerKBody.addInstruction(Instruction{"br", {lowerKHeaderName}});

  BasicBlock lowerLatch{lowerLatchName};
  lowerLatch.addInstruction(
      Instruction{"getelementptr",
                  {candidate.matrixBase, candidate.rowIndex},
                  diagonalRow});
  lowerLatch.addInstruction(Instruction{
      "getelementptr", {diagonalRow, candidate.rowIndex}, diagonalAddress});
  lowerLatch.addInstruction(
      Instruction{"load", {"int", diagonalAddress}, diagonal});
  lowerLatch.addInstruction(
      Instruction{"sdiv", {lowerAcc, diagonal}, quotient});
  lowerLatch.addInstruction(
      Instruction{"store", {"int", quotient, lowerDestination}});
  lowerLatch.addInstruction(Instruction{"add", {lowerRow, "1"}, lowerRowNext});
  lowerLatch.addInstruction(Instruction{"br", {lowerHeaderName}});

  BasicBlock waveLatch{waveLatchName};
  waveLatch.addInstruction(
      Instruction{"add", {candidate.rowIndex, "1"}, rowNext});
  waveLatch.addInstruction(Instruction{"br", {waveHeaderName}});

  const std::unordered_set<std::string> removed(candidate.removedBlocks.begin(),
                                                candidate.removedBlocks.end());
  function.blocks().erase(
      std::remove_if(function.blocks().begin(), function.blocks().end(),
                     [&](const BasicBlock &block) {
                       return removed.find(block.name()) != removed.end();
                     }),
      function.blocks().end());

  auto insertion = std::find_if(
      function.blocks().begin(), function.blocks().end(),
      [&](const BasicBlock &block) { return block.name() == candidate.exit; });
  insertion = function.blocks().insert(insertion, std::move(waveHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(upperSetup));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(upperHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(upperBody));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(upperKHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(upperKBody));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(upperLatch));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(packSetup));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(packHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(packBody));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(lowerSetup));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(lowerHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(lowerBody));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(lowerKHeader));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(lowerKBody));
  ++insertion;
  insertion = function.blocks().insert(insertion, std::move(lowerLatch));
  ++insertion;
  function.blocks().insert(insertion, std::move(waveLatch));
  return true;
}

} // namespace

void LoopWavefrontColumnPackingPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_LOOP_WAVEFRONT_COLUMN_PACKING") != nullptr)
    return;
  for (auto &function : module.functions()) {
    std::optional<Candidate> candidate;
    for (const auto &block : function.blocks()) {
      candidate = matchCandidate(function, block);
      if (candidate.has_value())
        break;
    }
    if (candidate.has_value())
      applyCandidate(function, *candidate);
  }
}

} // namespace compiler::midend::pass
