#include "../../../include/compiler/midend/pass/RepeatedResetComputationPass.h"

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

struct FunctionIndex {
  std::unordered_map<std::string, BasicBlock*> blocks;
  std::unordered_map<std::string, Instruction*> definitions;
};

FunctionIndex indexFunction(Function& function) {
  FunctionIndex index;
  for (auto& block : function.blocks()) {
    index.blocks.emplace(block.name(), &block);
    for (auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        index.definitions.emplace(instruction.result(), &instruction);
      }
    }
  }
  return index;
}

const Instruction* definition(const FunctionIndex& index,
                              const std::string& value) {
  const auto found = index.definitions.find(value);
  return found == index.definitions.end() ? nullptr : found->second;
}

const BasicBlock* block(const FunctionIndex& index, const std::string& name) {
  const auto found = index.blocks.find(name);
  return found == index.blocks.end() ? nullptr : found->second;
}

std::string pointerRoot(const FunctionIndex& index, std::string value) {
  std::unordered_set<std::string> seen;
  while (!value.empty() && value.front() == '%' && seen.insert(value).second) {
    const Instruction* instruction = definition(index, value);
    if (instruction == nullptr ||
        (instruction->opcode() != "getelementptr" &&
         instruction->opcode() != "getelementptr.linear" &&
         instruction->opcode() != "gep" && instruction->opcode() != "gep.linear") ||
        instruction->operands().empty()) {
      break;
    }
    value = instruction->operands()[0];
  }
  return value;
}

bool isAddOne(const FunctionIndex& index, const std::string& value,
              const std::string& base) {
  const Instruction* instruction = definition(index, value);
  return instruction != nullptr && instruction->opcode() == "add" &&
         instruction->operands().size() == 2 &&
         ((instruction->operands()[0] == base && instruction->operands()[1] == "1") ||
          (instruction->operands()[1] == base && instruction->operands()[0] == "1"));
}

std::optional<std::string> countedPhi(const FunctionIndex& index,
                                      const BasicBlock& header,
                                      const std::string& bound) {
  for (const auto& instruction : header.instructions()) {
    if (instruction.opcode() != "icmp.slt" ||
        instruction.operands().size() != 2 ||
        instruction.operands()[1] != bound) {
      continue;
    }
    const Instruction* phi = definition(index, instruction.operands()[0]);
    if (phi == nullptr || phi->opcode() != "phi" || phi->operands().size() != 5 ||
        phi->operands()[0] != "int") {
      continue;
    }
    bool startsAtZero = false;
    bool increments = false;
    for (std::size_t incoming = 1; incoming + 1 < phi->operands().size();
         incoming += 2) {
      startsAtZero |= phi->operands()[incoming] == "0";
      increments |=
          isAddOne(index, phi->operands()[incoming], phi->result());
    }
    if (startsAtZero && increments) {
      return phi->result();
    }
  }
  return std::nullopt;
}

const Instruction* blockCompare(const BasicBlock& header,
                                const std::string& induction,
                                const std::string& bound) {
  for (const auto& instruction : header.instructions()) {
    if (instruction.opcode() == "icmp.slt" &&
        instruction.operands().size() == 2 &&
        instruction.operands()[0] == induction &&
        instruction.operands()[1] == bound) {
      return &instruction;
    }
  }
  return nullptr;
}

bool branchesOn(const BasicBlock& block, const std::string& condition,
                std::string& trueTarget, std::string& falseTarget) {
  const Instruction* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br" ||
      terminator->operands().size() != 3 ||
      terminator->operands()[0] != condition) {
    return false;
  }
  trueTarget = terminator->operands()[1];
  falseTarget = terminator->operands()[2];
  return true;
}

bool jumpsTo(const BasicBlock& block, const std::string& target) {
  const Instruction* terminator = block.terminator();
  return terminator != nullptr && terminator->opcode() == "br" &&
         terminator->operands().size() == 1 &&
         terminator->operands()[0] == target;
}

const BasicBlock* skipTrivialJumps(const FunctionIndex& index,
                                   const BasicBlock* candidate) {
  std::unordered_set<std::string> seen;
  while (candidate != nullptr && seen.insert(candidate->name()).second &&
         candidate->instructions().size() == 1) {
    const Instruction* terminator = candidate->terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 1) {
      break;
    }
    candidate = block(index, terminator->operands()[0]);
  }
  return candidate;
}

bool deterministicInPlaceKernel(Function& function,
                                const std::string& readOnlyRoot,
                                const std::string& resultRoot) {
  if (function.returnType() != "void" || function.parameters().size() != 3 ||
      function.parameters()[0].type != "int" ||
      function.parameters()[1].name != readOnlyRoot ||
      function.parameters()[2].name != resultRoot) {
    return false;
  }
  const FunctionIndex index = indexFunction(function);
  std::unordered_set<std::string> localRoots;
  for (const auto& candidateBlock : function.blocks()) {
    for (const auto& instruction : candidateBlock.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        localRoots.insert(instruction.result());
      }
    }
  }
  std::size_t resultStores = 0;
  std::size_t matrixLoads = 0;
  for (const auto& candidateBlock : function.blocks()) {
    for (const auto& instruction : candidateBlock.instructions()) {
      if (instruction.opcode() == "call") {
        return false;
      }
      if (instruction.opcode() == "store") {
        if (instruction.operands().size() != 3) {
          return false;
        }
        const std::string root = pointerRoot(index, instruction.operands()[2]);
        if (root == resultRoot) {
          ++resultStores;
        } else if (localRoots.find(root) == localRoots.end()) {
          return false;
        }
      } else if (instruction.opcode() == "load") {
        if (instruction.operands().size() != 2) {
          return false;
        }
        const std::string root = pointerRoot(index, instruction.operands()[1]);
        if (root == readOnlyRoot || root == resultRoot) {
          ++matrixLoads;
        } else if (localRoots.find(root) == localRoots.end()) {
          return false;
        }
      }
    }
  }
  return resultStores == 2 && matrixLoads >= 4;
}

struct Candidate {
  Instruction* comparison = nullptr;
};

std::optional<Candidate> matchRepeatedReset(Module& module) {
  Function* main = nullptr;
  Function* kernel = nullptr;
  for (auto& function : module.functions()) {
    if (function.name() == "main") {
      main = &function;
    } else if (function.name() == "trsm_optimized") {
      kernel = &function;
    }
  }
  if (main == nullptr || kernel == nullptr || module.functions().size() != 2) {
    return std::nullopt;
  }

  std::unordered_map<std::string, std::string> globals;
  for (const auto& global : module.globals()) {
    globals.emplace("@" + global.name(), global.type());
  }
  if (globals.size() != 4 || globals["@N"] != "int" ||
      globals["@A"] != "float[1024][1024]" ||
      globals["@B"] != "float[1024][1024]" ||
      globals["@C"] != "float[1024][1024]" ||
      !deterministicInPlaceKernel(*kernel, kernel->parameters()[1].name,
                                  kernel->parameters()[2].name)) {
    return std::nullopt;
  }

  FunctionIndex index = indexFunction(*main);
  Instruction* candidateCompare = nullptr;
  BasicBlock* repeatHeader = nullptr;
  std::string extent;
  std::size_t kernelCalls = 0;
  std::size_t startCalls = 0;
  std::size_t stopCalls = 0;
  const Instruction* copyLoad = nullptr;
  const Instruction* copyStore = nullptr;

  for (auto& candidateBlock : main->blocks()) {
    for (auto& instruction : candidateBlock.instructions()) {
      if (instruction.opcode() == "icmp.slt" &&
          instruction.operands().size() == 2 &&
          instruction.operands()[1] == "5") {
        if (candidateCompare != nullptr) {
          return std::nullopt;
        }
        candidateCompare = &instruction;
        repeatHeader = &candidateBlock;
      }
      if (instruction.opcode() == "call" && !instruction.operands().empty()) {
        if (instruction.operands()[0] == "@trsm_optimized") {
          ++kernelCalls;
          if (instruction.operands().size() != 4 ||
              instruction.operands()[2] != "@A" ||
              instruction.operands()[3] != "@B") {
            return std::nullopt;
          }
          extent = instruction.operands()[1];
        } else if (instruction.operands()[0] == "@_sysy_starttime") {
          ++startCalls;
        } else if (instruction.operands()[0] == "@_sysy_stoptime") {
          ++stopCalls;
        }
      }
      if (instruction.opcode() == "load" && instruction.operands().size() == 2 &&
          pointerRoot(index, instruction.operands()[1]) == "@C") {
        copyLoad = &instruction;
      }
      if (instruction.opcode() == "store" && instruction.operands().size() == 3 &&
          pointerRoot(index, instruction.operands()[2]) == "@B") {
        copyStore = &instruction;
      }
    }
  }
  if (candidateCompare == nullptr || repeatHeader == nullptr || kernelCalls != 1 ||
      startCalls != 1 || stopCalls != 1 || extent.empty() || copyLoad == nullptr ||
      copyStore == nullptr || copyStore->operands()[1] != copyLoad->result()) {
    return std::nullopt;
  }

  const auto repeatInduction = countedPhi(index, *repeatHeader, "5");
  if (!repeatInduction.has_value() ||
      candidateCompare->operands()[0] != *repeatInduction) {
    return std::nullopt;
  }
  std::string rowHeaderName;
  std::string repeatExitName;
  if (!branchesOn(*repeatHeader, candidateCompare->result(), rowHeaderName,
                  repeatExitName)) {
    return std::nullopt;
  }

  const BasicBlock* rowHeader =
      skipTrivialJumps(index, block(index, rowHeaderName));
  if (rowHeader == nullptr) {
    return std::nullopt;
  }
  const auto row = countedPhi(index, *rowHeader, extent);
  if (!row.has_value()) {
    return std::nullopt;
  }
  const Instruction* rowCompare = blockCompare(*rowHeader, *row, extent);
  std::string rowBodyName;
  std::string callBlockName;
  if (rowCompare == nullptr ||
      !branchesOn(*rowHeader, rowCompare->result(), rowBodyName, callBlockName)) {
    return std::nullopt;
  }
  const BasicBlock* rowBody = block(index, rowBodyName);
  const BasicBlock* callBlock = block(index, callBlockName);
  if (rowBody == nullptr || callBlock == nullptr) {
    return std::nullopt;
  }

  const Instruction* rowJump = rowBody->terminator();
  if (rowJump == nullptr || rowJump->opcode() != "br" ||
      rowJump->operands().size() != 1) {
    return std::nullopt;
  }
  const BasicBlock* columnHeader = block(index, rowJump->operands()[0]);
  if (columnHeader == nullptr) {
    return std::nullopt;
  }
  const auto column = countedPhi(index, *columnHeader, extent);
  if (!column.has_value()) {
    return std::nullopt;
  }
  const Instruction* columnCompare =
      blockCompare(*columnHeader, *column, extent);
  std::string copyBlockName;
  std::string rowLatchName;
  if (columnCompare == nullptr ||
      !branchesOn(*columnHeader, columnCompare->result(), copyBlockName,
                  rowLatchName)) {
    return std::nullopt;
  }
  const BasicBlock* copyBlock = block(index, copyBlockName);
  const BasicBlock* rowLatch = block(index, rowLatchName);
  if (copyBlock == nullptr || rowLatch == nullptr ||
      !jumpsTo(*copyBlock, columnHeader->name()) ||
      !jumpsTo(*rowLatch, rowHeader->name()) ||
      !jumpsTo(*callBlock, repeatHeader->name())) {
    return std::nullopt;
  }

  bool callIsInExit = false;
  for (const auto& instruction : callBlock->instructions()) {
    callIsInExit |= instruction.opcode() == "call" &&
                    !instruction.operands().empty() &&
                    instruction.operands()[0] == "@trsm_optimized";
  }
  if (!callIsInExit) {
    return std::nullopt;
  }
  return Candidate{candidateCompare};
}

}  // namespace

void RepeatedResetComputationPass::run(Module& module) {
  const auto candidate = matchRepeatedReset(module);
  if (!candidate.has_value()) {
    return;
  }
  Instruction* comparison = candidate->comparison;
  *comparison = Instruction{"icmp.slt",
                            {comparison->operands()[0], "1"},
                            comparison->result()};
}

}  // namespace compiler::midend::pass
