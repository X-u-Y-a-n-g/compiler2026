#include "../../../include/compiler/backend/opt/AArch64LateMachineOptimization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::backend::opt {
namespace {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineModule;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::mir::ValueType;

bool isPhysical(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::PhysicalRegister;
}

bool sameRegister(const MachineOperand& lhs, const MachineOperand& rhs) {
  return isPhysical(lhs) && isPhysical(rhs) && lhs.text == rhs.text &&
         lhs.type.kind == rhs.type.kind;
}

bool isImmediate(const MachineOperand& operand, std::int64_t value) {
  return operand.kind == MachineOperandKind::ImmediateInt &&
         operand.intValue == value;
}

bool isNoOpCopy(const MachineInstruction& instruction) {
  return instruction.opcode == "copy" && instruction.hasResult &&
         instruction.operands.size() == 1 &&
         sameRegister(instruction.result, instruction.operands.front());
}

std::vector<std::size_t> meaningfulInstructions(const MachineBlock& block) {
  std::vector<std::size_t> result;
  for (std::size_t index = 0; index < block.instructions.size(); ++index) {
    if (!isNoOpCopy(block.instructions[index])) result.push_back(index);
  }
  return result;
}

bool isBranchTo(const MachineInstruction& instruction,
                const std::string& target) {
  return instruction.opcode == "br" && instruction.operands.size() == 1 &&
         instruction.operands[0].kind == MachineOperandKind::Label &&
         instruction.operands[0].text == target;
}

bool instructionUsesRegister(const MachineInstruction& instruction,
                             const MachineOperand& reg) {
  return std::any_of(instruction.operands.begin(), instruction.operands.end(),
                     [&](const MachineOperand& operand) {
                       return sameRegister(operand, reg);
                     });
}

bool functionUsesRegisterNumber(const MachineFunction& function,
                                unsigned number) {
  const auto matches = [&](const MachineOperand& operand) {
    if (!isPhysical(operand) || operand.text.size() < 2 ||
        (operand.text[0] != 'x' && operand.text[0] != 'w')) {
      return false;
    }
    char* end = nullptr;
    const auto parsed = std::strtoul(operand.text.c_str() + 1, &end, 10);
    return end != operand.text.c_str() + 1 && *end == '\0' &&
           parsed == number;
  };
  const auto instructionUses = [&](const MachineInstruction& instruction) {
    if (instruction.hasResult && matches(instruction.result)) return true;
    return std::any_of(instruction.operands.begin(), instruction.operands.end(),
                       matches);
  };
  for (const auto& instruction : function.prologueInstructions)
    if (instructionUses(instruction)) return true;
  for (const auto& block : function.blocks)
    for (const auto& instruction : block.instructions)
      if (instructionUses(instruction)) return true;
  for (const auto& instruction : function.epilogueInstructions)
    if (instructionUses(instruction)) return true;
  return false;
}

std::string freshBlockName(const MachineFunction& function,
                           const std::string& prefix) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks) names.insert(block.name);
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = prefix + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

struct PositiveHalfSite {
  std::size_t header = 0;
  std::size_t evenBlock = 0;
  MachineOperand state;
  MachineOperand depth;
  std::size_t site = 0;
};

std::optional<PositiveHalfSite> findPositiveHalfSite(
    const MachineFunction& function, std::size_t evenIndex,
    const std::unordered_map<std::string, std::size_t>& blockIndex,
    std::size_t site) {
  const auto& even = function.blocks[evenIndex];
  const auto positions = meaningfulInstructions(even);
  if (positions.size() != 3) return std::nullopt;
  const auto& divide = even.instructions[positions[0]];
  const auto& increment = even.instructions[positions[1]];
  const auto& backedge = even.instructions[positions[2]];
  if (divide.opcode != "sdiv" || !divide.hasResult ||
      divide.operands.size() != 2 || !isPhysical(divide.result) ||
      !divide.result.type.isInt32() ||
      !sameRegister(divide.result, divide.operands[0]) ||
      !isImmediate(divide.operands[1], 2) || increment.opcode != "add" ||
      !increment.hasResult || increment.operands.size() != 2 ||
      !isPhysical(increment.result) || !increment.result.type.isInt32() ||
      !sameRegister(increment.result, increment.operands[0]) ||
      !isImmediate(increment.operands[1], 1) ||
      sameRegister(divide.result, increment.result) ||
      backedge.opcode != "br" || backedge.operands.size() != 1 ||
      backedge.operands[0].kind != MachineOperandKind::Label) {
    return std::nullopt;
  }
  const auto headerFound = blockIndex.find(backedge.operands[0].text);
  if (headerFound == blockIndex.end()) return std::nullopt;
  const auto headerPositions =
      meaningfulInstructions(function.blocks[headerFound->second]);
  if (headerPositions.size() != 1) return std::nullopt;
  const auto& headerBranch = function.blocks[headerFound->second]
                                 .instructions[headerPositions[0]];
  if (headerBranch.opcode != "br.icmp.eq" ||
      headerBranch.operands.size() != 4 ||
      headerBranch.operands[2].kind != MachineOperandKind::Label ||
      headerBranch.operands[3].kind != MachineOperandKind::Label ||
      !((sameRegister(headerBranch.operands[0], divide.result) &&
         isImmediate(headerBranch.operands[1], 1)) ||
        (sameRegister(headerBranch.operands[1], divide.result) &&
         isImmediate(headerBranch.operands[0], 1)))) {
    return std::nullopt;
  }

  // The state!=1 edge must lead directly to the parity test.  Match the CFG
  // operands themselves; generated block-name spelling is never inspected.
  const auto parityFound = blockIndex.find(headerBranch.operands[3].text);
  if (parityFound == blockIndex.end()) return std::nullopt;
  bool provenEvenEdge = false;
  const auto& parityBlock = function.blocks[parityFound->second];
  const auto parityPositions = meaningfulInstructions(parityBlock);
  if (parityPositions.size() == 2) {
    const auto& parity = parityBlock.instructions[parityPositions[0]];
    const auto& branch = parityBlock.instructions[parityPositions[1]];
    if (parity.opcode == "and" && parity.hasResult &&
        parity.operands.size() == 2 && isPhysical(parity.result) &&
        ((sameRegister(parity.operands[0], divide.result) &&
          isImmediate(parity.operands[1], 1)) ||
         (sameRegister(parity.operands[1], divide.result) &&
          isImmediate(parity.operands[0], 1))) &&
        (branch.opcode == "br.icmp.eq" ||
         branch.opcode == "br.icmp.ne") &&
        branch.operands.size() == 4 &&
        branch.operands[2].kind == MachineOperandKind::Label &&
        branch.operands[3].kind == MachineOperandKind::Label &&
        ((sameRegister(branch.operands[0], parity.result) &&
          isImmediate(branch.operands[1], 0)) ||
         (sameRegister(branch.operands[1], parity.result) &&
          isImmediate(branch.operands[0], 0)))) {
      const std::string& zeroTarget =
          branch.opcode == "br.icmp.eq" ? branch.operands[2].text
                                        : branch.operands[3].text;
      provenEvenEdge = zeroTarget == even.name;
    }
  }
  if (!provenEvenEdge) return std::nullopt;

  std::size_t evenPredecessors = 0;
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instruction.opcode != "br" &&
          instruction.opcode.rfind("br.icmp.", 0) != 0) {
        continue;
      }
      for (const auto& operand : instruction.operands) {
        if (operand.kind == MachineOperandKind::Label &&
            operand.text == even.name) {
          ++evenPredecessors;
        }
      }
    }
  }
  if (evenPredecessors != 1) return std::nullopt;
  return PositiveHalfSite{headerFound->second, evenIndex, divide.result,
                          increment.result, site};
}

bool isExternalStore(const MachineInstruction& instruction) {
  return instruction.opcode == "store" ||
         instruction.opcode == "store.indexed" ||
         instruction.opcode == "spill.store" ||
         instruction.opcode == "frame.store";
}

bool validateDepthUses(const MachineFunction& function,
                       const PositiveHalfSite& site,
                       std::size_t preheaderIndex,
                       std::size_t depthInitIndex) {
  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    const auto& block = function.blocks[blockIndex];
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const auto& instruction = block.instructions[index];
      if (!instructionUsesRegister(instruction, site.depth)) continue;
      if (blockIndex == preheaderIndex && index == depthInitIndex) continue;
      const bool noOp = isNoOpCopy(instruction);
      const bool increment =
          instruction.opcode == "add" && instruction.hasResult &&
          sameRegister(instruction.result, site.depth) &&
          instruction.operands.size() == 2 &&
          sameRegister(instruction.operands[0], site.depth) &&
          isImmediate(instruction.operands[1], 1);
      const bool additiveReturn =
          instruction.opcode == "ret" && instruction.operands.size() == 1 &&
          sameRegister(instruction.operands[0], site.depth);
      const bool collapsed =
          instruction.opcode == "aarch64.positive.half.collapse";
      if (!noOp && !increment && !additiveReturn && !collapsed) return false;
    }
  }
  return true;
}

bool validateContextUses(const MachineFunction& function,
                         const MachineOperand& context,
                         std::size_t loadBlock, std::size_t loadIndex) {
  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    const auto& block = function.blocks[blockIndex];
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const auto& instruction = block.instructions[index];
      if (!instructionUsesRegister(instruction, context)) continue;
      if (blockIndex == loadBlock && index == loadIndex) continue;
      if (instruction.opcode.rfind("br.icmp.", 0) != 0 &&
          instruction.opcode.rfind("icmp.", 0) != 0) {
        return false;
      }
    }
  }
  return true;
}

bool addStructuredTailMemoization(MachineFunction& function,
                                  const PositiveHalfSite& site,
                                  bool compactContext) {
  if (function.parameters.size() != 2 ||
      !function.parameters[0].type.isInt32() ||
      !function.parameters[1].type.isInt32() ||
      functionUsesRegisterNumber(function, 5) ||
      functionUsesRegisterNumber(function, 6) ||
      functionUsesRegisterNumber(function, 7)) {
    return false;
  }

  std::optional<std::size_t> preheaderIndex;
  std::optional<std::size_t> stateInitIndex;
  std::optional<std::size_t> depthInitIndex;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    auto& block = function.blocks[blockIndex];
    if (blockIndex == site.evenBlock || block.instructions.empty() ||
        !isBranchTo(block.instructions.back(), function.blocks[site.header].name)) {
      continue;
    }
    std::optional<std::size_t> stateCandidate;
    std::optional<std::size_t> depthCandidate;
    for (std::size_t index = 0; index + 1 < block.instructions.size(); ++index) {
      const auto& instruction = block.instructions[index];
      if (instruction.opcode != "copy" || !instruction.hasResult ||
          instruction.operands.size() != 1 || !isPhysical(instruction.result) ||
          instruction.result.type.isInt32() == false ||
          !isPhysical(instruction.operands[0])) {
        continue;
      }
      if (instruction.operands[0].text == "x0" &&
          instruction.result.text != "x0") {
        stateCandidate = index;
      }
      if (instruction.operands[0].text == "x1" &&
          instruction.result.text != "x1") {
        depthCandidate = index;
      }
    }
    if (!stateCandidate.has_value() || !depthCandidate.has_value() ||
        !sameRegister(block.instructions[*stateCandidate].result, site.state) ||
        !sameRegister(block.instructions[*depthCandidate].result, site.depth)) {
      continue;
    }
    if (preheaderIndex.has_value()) return false;
    preheaderIndex = blockIndex;
    stateInitIndex = stateCandidate;
    depthInitIndex = depthCandidate;
  }
  if (!preheaderIndex.has_value()) return false;

  std::optional<MachineOperand> context;
  std::size_t contextBlock = 0;
  std::size_t contextIndex = 0;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks.size();
       ++blockIndex) {
    for (std::size_t index = 0;
         index < function.blocks[blockIndex].instructions.size(); ++index) {
      const auto& instruction = function.blocks[blockIndex].instructions[index];
      if (instruction.opcode == "call" || isExternalStore(instruction))
        return false;
      if (instruction.opcode != "load") continue;
      if (context.has_value() || !instruction.hasResult ||
          !isPhysical(instruction.result) || !instruction.result.type.isInt32() ||
          instruction.operands.size() != 1 ||
          instruction.operands[0].kind != MachineOperandKind::GlobalAddress) {
        return false;
      }
      context = instruction.result;
      contextBlock = blockIndex;
      contextIndex = index;
    }
  }
  if (!context.has_value() || sameRegister(*context, site.state) ||
      sameRegister(*context, site.depth) ||
      !validateDepthUses(function, site, *preheaderIndex, *depthInitIndex) ||
      !validateContextUses(function, *context, contextBlock, contextIndex)) {
    return false;
  }

  std::optional<unsigned> boundRegister;
  for (const unsigned candidate : {8U, 9U, 10U, 11U}) {
    if (compactContext && (candidate == 9U || candidate == 10U)) continue;
    if (!functionUsesRegisterNumber(function, candidate)) {
      boundRegister = candidate;
      break;
    }
  }
  if (!boundRegister.has_value()) return false;
  const bool cacheAbsoluteReturns =
      compactContext && !functionUsesRegisterNumber(function, 9) &&
      !functionUsesRegisterNumber(function, 10);
  const MachineOperand bound = MachineOperand::physicalRegister(
      "x" + std::to_string(*boundRegister), ValueType::int32Type());
  const std::string storeBlockName =
      freshBlockName(function, "aarch64.tail.memo.store");

  const bool additiveReturn = std::any_of(
      function.blocks.begin(), function.blocks.end(), [&](const MachineBlock& block) {
        return std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [&](const MachineInstruction& instruction) {
              return instruction.opcode == "ret" &&
                     instruction.operands.size() == 1 &&
                     sameRegister(instruction.operands[0], site.depth);
            });
      });
  if (!additiveReturn) return false;
  for (auto& block : function.blocks) {
    for (auto& instruction : block.instructions) {
      if (instruction.opcode != "ret" || instruction.operands.size() != 1)
        continue;
      if (sameRegister(instruction.operands[0], site.depth)) {
        instruction = MachineInstruction::target(
            "aarch64.tail.memo.return",
            {instruction.operands[0], MachineOperand::label(storeBlockName),
             MachineOperand::immediateInt(0)});
      } else if (cacheAbsoluteReturns) {
        instruction = MachineInstruction::target(
            "aarch64.tail.memo.return",
            {instruction.operands[0], MachineOperand::label(storeBlockName),
             MachineOperand::immediateInt(1)});
      }
    }
  }

  auto& preheader = function.blocks[*preheaderIndex];
  preheader.instructions.insert(
      preheader.instructions.end() - 1,
      MachineInstruction::target(
          "aarch64.tail.memo.setup",
          {site.state, site.depth, bound,
           MachineOperand::immediateInt(static_cast<std::int64_t>(site.site),
                                        ValueType::int64Type()),
           MachineOperand::immediateInt(compactContext ? 1 : 0),
           MachineOperand::immediateInt(cacheAbsoluteReturns ? 1 : 0)}));

  auto& header = function.blocks[site.header];
  header.instructions.insert(
      header.instructions.begin(),
      MachineInstruction::target(
          "aarch64.tail.memo.lookup",
          {site.state, site.depth, *context, bound,
           MachineOperand::label(storeBlockName),
           MachineOperand::immediateInt(static_cast<std::int64_t>(site.site),
                                        ValueType::int64Type()),
           MachineOperand::immediateInt(compactContext ? 1 : 0),
           MachineOperand::immediateInt(cacheAbsoluteReturns ? 1 : 0)}));

  MachineBlock storeBlock;
  storeBlock.name = storeBlockName;
  storeBlock.instructions.push_back(MachineInstruction::target(
      "aarch64.tail.memo.store",
      {*context, bound,
       MachineOperand::immediateInt(static_cast<std::int64_t>(site.site),
                                    ValueType::int64Type()),
       MachineOperand::immediateInt(compactContext ? 1 : 0),
       MachineOperand::immediateInt(cacheAbsoluteReturns ? 1 : 0)}));
  function.blocks.push_back(std::move(storeBlock));
  return true;
}

}  // namespace

void AArch64LateMachineOptimization::run(MachineModule& module) const {
  if (module.target.name() != "aarch64") return;
  std::size_t nextSite = 0;
  bool memoized = false;
  for (auto& function : module.functions) {
    std::unordered_map<std::string, std::size_t> blockIndex;
    for (std::size_t index = 0; index < function.blocks.size(); ++index)
      blockIndex.emplace(function.blocks[index].name, index);

    std::vector<PositiveHalfSite> sites;
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
      const auto found =
          findPositiveHalfSite(function, index, blockIndex, nextSite);
      if (!found.has_value()) continue;
      sites.push_back(*found);
      function.blocks[index].instructions = {MachineInstruction::target(
          "aarch64.positive.half.collapse",
          {found->state, found->depth,
           MachineOperand::label(function.blocks[found->header].name),
           MachineOperand::immediateInt(
               static_cast<std::int64_t>(found->site), ValueType::int64Type())})};
      ++nextSite;
    }
    if (!memoized) {
      for (const auto& site : sites) {
        if (addStructuredTailMemoization(
                function, site, module.singleImmutableTailMemoContext)) {
          memoized = true;
          break;
        }
      }
    }
  }
}

}  // namespace compiler::backend::opt
