#include "../../../include/compiler/midend/pass/LazyAffineCopyResolutionPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
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

struct Definition {
  const Instruction* instruction = nullptr;
  std::size_t block = 0;
};
using DefinitionMap = std::unordered_map<std::string, Definition>;

struct PointerAccess {
  std::string base;
  std::string index;
};

struct SimpleLoop {
  std::string header;
  std::string body;
  std::string preheader;
  std::string backedge;
  std::string exit;
  std::string induction;
  std::string bound;
  std::size_t headerIndex = 0;
  std::size_t bodyIndex = 0;
};

struct CopyKernel {
  std::size_t functionIndex = 0;
};

struct Candidate {
  std::size_t functionIndex = 0;
  SimpleLoop calls;
  SimpleLoop consumer;
  std::string count;
  std::string length;
  std::string data;
  std::string parameters;
  std::string consumerPointer;
  std::size_t dataCapacity = 0;
  std::size_t parameterCapacity = 0;
};

DefinitionMap definitionsOf(const Function& function) {
  DefinitionMap result;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult()) {
        result.emplace(instruction.result(), Definition{&instruction, block});
      }
    }
  }
  return result;
}

std::unordered_map<std::string, std::size_t> blockIndices(
    const Function& function) {
  std::unordered_map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    result.emplace(function.blocks()[index].name(), index);
  }
  return result;
}

std::unordered_set<std::size_t> reachableFrom(const Function& function,
                                              std::size_t start) {
  const auto indices = blockIndices(function);
  std::unordered_set<std::size_t> reachable{start};
  std::vector<std::size_t> worklist{start};
  while (!worklist.empty()) {
    const auto block = worklist.back();
    worklist.pop_back();
    const auto* terminator = function.blocks()[block].terminator();
    if (terminator == nullptr) continue;
    for (const auto& operand : terminator->operands()) {
      const auto successor = indices.find(operand);
      if (successor != indices.end() &&
          reachable.insert(successor->second).second) {
        worklist.push_back(successor->second);
      }
    }
  }
  return reachable;
}

bool isInteger(const std::string& value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

bool isGep(const Instruction& instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "getelementptr.linear" ||
         instruction.opcode() == "gep" ||
         instruction.opcode() == "gep.linear";
}

std::optional<PointerAccess> pointerAccess(
    const std::string& pointer, const DefinitionMap& definitions) {
  const auto found = definitions.find(pointer);
  if (found == definitions.end() || !isGep(*found->second.instruction) ||
      found->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  std::string base = found->second.instruction->operands()[0];
  const std::string index = found->second.instruction->operands()[1];
  std::unordered_set<std::string> visiting;
  while (!base.empty() && base.front() == '%') {
    if (!visiting.insert(base).second) return std::nullopt;
    const auto definition = definitions.find(base);
    if (definition == definitions.end() ||
        definition->second.instruction->opcode() != "cast" ||
        definition->second.instruction->operands().empty()) {
      break;
    }
    base = definition->second.instruction->operands().front();
  }
  return PointerAccess{base, index};
}

std::string pointerRoot(const std::string& pointer,
                        const DefinitionMap& definitions) {
  std::string root = pointer;
  std::unordered_set<std::string> visiting;
  while (!root.empty() && root.front() == '%') {
    if (!visiting.insert(root).second) return {};
    const auto definition = definitions.find(root);
    if (definition == definitions.end()) break;
    const auto& instruction = *definition->second.instruction;
    if ((instruction.opcode() != "cast" && !isGep(instruction)) ||
        instruction.operands().empty()) {
      break;
    }
    root = instruction.operands().front();
  }
  return root;
}

bool matchBinary(const std::string& value, const std::string& opcode,
                 const std::string& lhs, const std::string& rhs,
                 const DefinitionMap& definitions) {
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != opcode ||
      found->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto& operands = found->second.instruction->operands();
  return (operands[0] == lhs && operands[1] == rhs) ||
         (operands[0] == rhs && operands[1] == lhs);
}

bool matchProductPlus(const std::string& value, const std::string& lhs,
                      const std::string& rhs, const std::string& addend,
                      const DefinitionMap& definitions) {
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != "add" ||
      found->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto& operands = found->second.instruction->operands();
  return (operands[0] == addend &&
          matchBinary(operands[1], "mul", lhs, rhs, definitions)) ||
         (operands[1] == addend &&
          matchBinary(operands[0], "mul", lhs, rhs, definitions));
}

std::vector<SimpleLoop> simpleLoops(const Function& function,
                                    const DefinitionMap& definitions) {
  std::vector<SimpleLoop> result;
  const auto indices = blockIndices(function);
  for (std::size_t headerIndex = 0; headerIndex < function.blocks().size();
       ++headerIndex) {
    const auto& header = function.blocks()[headerIndex];
    const auto* branch = header.terminator();
    if (branch == nullptr || branch->opcode() != "br" ||
        branch->operands().size() != 3) {
      continue;
    }
    const auto condition = definitions.find(branch->operands()[0]);
    if (condition == definitions.end() ||
        condition->second.block != headerIndex ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2) {
      continue;
    }
    const std::string induction =
        condition->second.instruction->operands()[0];
    const std::string bound = condition->second.instruction->operands()[1];
    const auto phi = definitions.find(induction);
    if (phi == definitions.end() || phi->second.block != headerIndex ||
        phi->second.instruction->opcode() != "phi" ||
        phi->second.instruction->operands().size() != 5) {
      continue;
    }
    std::string preheader;
    std::string backedge;
    std::string increment;
    const auto& phiOperands = phi->second.instruction->operands();
    for (std::size_t index = 1; index + 1 < phiOperands.size(); index += 2) {
      if (isInteger(phiOperands[index], 0)) {
        preheader = phiOperands[index + 1];
      } else {
        increment = phiOperands[index];
        backedge = phiOperands[index + 1];
      }
    }
    const auto body = indices.find(branch->operands()[1]);
    const auto exit = indices.find(branch->operands()[2]);
    const auto back = indices.find(backedge);
    const auto incrementDefinition = definitions.find(increment);
    if (preheader.empty() || body == indices.end() || exit == indices.end() ||
        back == indices.end() || incrementDefinition == definitions.end() ||
        incrementDefinition->second.instruction->opcode() != "add" ||
        incrementDefinition->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto& add = incrementDefinition->second.instruction->operands();
    const auto* backBranch = function.blocks()[back->second].terminator();
    if (!((add[0] == induction && isInteger(add[1], 1)) ||
          (add[1] == induction && isInteger(add[0], 1))) ||
        backBranch == nullptr || backBranch->operands().size() != 1 ||
        backBranch->operands()[0] != header.name()) {
      continue;
    }
    result.push_back(SimpleLoop{header.name(),
                                branch->operands()[1],
                                preheader,
                                backedge,
                                branch->operands()[2],
                                induction,
                                bound,
                                headerIndex,
                                body->second});
  }
  return result;
}

bool valueUnused(const Function& function, const Instruction& definition) {
  if (!definition.hasResult()) return true;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (&instruction == &definition) continue;
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    definition.result()) != instruction.operands().end()) {
        return false;
      }
    }
  }
  return true;
}

std::optional<CopyKernel> analyzeKernel(const Function& function,
                                        std::size_t functionIndex) {
  if (function.parameters().size() != 3 ||
      function.parameters()[0].type != "int" ||
      (function.parameters()[1].type != "ptr" &&
       function.parameters()[1].type.find("[]") == std::string::npos) ||
      function.parameters()[2].type != "int") {
    return std::nullopt;
  }
  const std::string count = function.parameters()[0].name;
  const std::string data = function.parameters()[1].name;
  const std::string rowSize = function.parameters()[2].name;
  const auto definitions = definitionsOf(function);
  const auto loops = simpleLoops(function, definitions);
  std::unordered_set<std::string> localAllocas;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        localAllocas.insert(instruction.result());
      }
    }
  }
  std::string columnSize;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "call") return std::nullopt;
      if (instruction.opcode() == "sdiv" &&
          instruction.operands().size() == 2 &&
          instruction.operands()[0] == count &&
          instruction.operands()[1] == rowSize) {
        if (!columnSize.empty()) return std::nullopt;
        columnSize = instruction.result();
      }
    }
  }
  if (columnSize.empty()) return std::nullopt;

  const Instruction* load = nullptr;
  const Instruction* store = nullptr;
  std::size_t loadBlock = 0;
  std::size_t storeBlock = 0;
  PointerAccess loadAccess;
  PointerAccess storeAccess;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto& block = function.blocks()[blockIndex];
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "load" &&
          instruction.opcode() != "store") {
        continue;
      }
      const auto& address = instruction.operands().back();
      const auto access = pointerAccess(address, definitions);
      const auto root = pointerRoot(address, definitions);
      if (localAllocas.find(root) != localAllocas.end()) {
        continue;
      }
      if (!access.has_value() || root != data) {
        return std::nullopt;
      }
      if (instruction.opcode() == "load") {
        if (load != nullptr) return std::nullopt;
        load = &instruction;
        loadBlock = blockIndex;
        loadAccess = *access;
      } else {
        if (store != nullptr) return std::nullopt;
        store = &instruction;
        storeBlock = blockIndex;
        storeAccess = *access;
      }
    }
  }
  if (load == nullptr || store == nullptr || store->operands().size() != 3 ||
      store->operands()[1] != load->result()) {
    return std::nullopt;
  }

  for (const auto& outer : loops) {
    if (outer.bound != columnSize) continue;
    for (const auto& inner : loops) {
      if (inner.header == outer.header ||
          inner.body != inner.backedge || loadBlock != inner.bodyIndex ||
          storeBlock != inner.bodyIndex ||
          !matchProductPlus(loadAccess.index, outer.induction, rowSize,
                            inner.induction, definitions) ||
          !matchProductPlus(storeAccess.index, inner.induction, columnSize,
                            outer.induction, definitions)) {
        continue;
      }
      const auto bound = definitions.find(inner.bound);
      if (bound == definitions.end() ||
          bound->second.instruction->opcode() != "select" ||
          bound->second.instruction->operands().size() != 3) {
        continue;
      }
      const auto& select = bound->second.instruction->operands();
      if (select[2] != rowSize ||
          !matchBinary(select[1], "add", outer.induction, "1",
                       definitions)) {
        continue;
      }
      const auto comparison = definitions.find(select[0]);
      if (comparison == definitions.end() ||
          comparison->second.instruction->opcode() != "icmp.slt" ||
          comparison->second.instruction->operands().size() != 2 ||
          comparison->second.instruction->operands()[0] != select[1] ||
          comparison->second.instruction->operands()[1] != rowSize) {
        continue;
      }
      return CopyKernel{functionIndex};
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> globalCapacity(const Module& module,
                                          const std::string& value) {
  if (value.empty() || value.front() != '@') return std::nullopt;
  for (const auto& global : module.globals()) {
    if ("@" + global.name() != value) continue;
    const auto open = global.type().find('[');
    const auto close = global.type().find(']', open);
    long long capacity = 0;
    if (open == std::string::npos || close == std::string::npos ||
        close <= open + 1) {
      return std::nullopt;
    }
    auto extent = global.type().substr(open + 1, close - open - 1);
    const auto separator = extent.find(" x ");
    if (separator != std::string::npos) extent.resize(separator);
    if (!detail::parseIntegerLiteral(extent, capacity) ||
        capacity <= 0) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(capacity);
  }
  return std::nullopt;
}

std::optional<Candidate> analyzeCaller(const Module& module,
                                       const CopyKernel& kernel) {
  const auto& callee = module.functions()[kernel.functionIndex];
  for (std::size_t functionIndex = 0;
       functionIndex < module.functions().size(); ++functionIndex) {
    if (functionIndex == kernel.functionIndex) continue;
    const auto& function = module.functions()[functionIndex];
    const auto definitions = definitionsOf(function);
    const auto loops = simpleLoops(function, definitions);
    for (const auto& callLoop : loops) {
      if (callLoop.body != callLoop.backedge) continue;
      const auto afterCalls = reachableFrom(function, callLoop.headerIndex);
      const Instruction* call = nullptr;
      for (const auto& instruction :
           function.blocks()[callLoop.bodyIndex].instructions()) {
        if (instruction.opcode() == "call" &&
            instruction.operands().size() == 4 &&
            instruction.operands()[0] == "@" + callee.name()) {
          if (call != nullptr) {
            call = nullptr;
            break;
          }
          call = &instruction;
        }
      }
      if (call == nullptr || !valueUnused(function, *call)) continue;
      bool callBodyHasOtherEffects = false;
      for (const auto& instruction :
           function.blocks()[callLoop.bodyIndex].instructions()) {
        if ((instruction.opcode() == "call" && &instruction != call) ||
            instruction.opcode() == "store") {
          callBodyHasOtherEffects = true;
        }
      }
      if (callBodyHasOtherEffects) continue;
      const std::string count = call->operands()[1];
      const std::string data = call->operands()[2];
      const auto rowDefinition = definitions.find(call->operands()[3]);
      if (rowDefinition == definitions.end() ||
          rowDefinition->second.instruction->opcode() != "load") {
        continue;
      }
      const auto rowAccess =
          pointerAccess(rowDefinition->second.instruction->operands().back(),
                        definitions);
      if (!rowAccess.has_value() ||
          rowAccess->index != callLoop.induction ||
          rowAccess->base == data) {
        continue;
      }
      const auto dataCapacity = globalCapacity(module, data);
      const auto parameterCapacity = globalCapacity(module, rowAccess->base);
      if (!dataCapacity.has_value() || !parameterCapacity.has_value()) {
        continue;
      }
      for (const auto& consumer : loops) {
        if (consumer.preheader != callLoop.exit ||
            consumer.bound != callLoop.bound ||
            consumer.body != consumer.backedge) {
          continue;
        }
        const Instruction* targetLoad = nullptr;
        std::string targetPointer;
        for (const auto& instruction :
             function.blocks()[consumer.bodyIndex].instructions()) {
          if (instruction.opcode() != "load") continue;
          const auto access =
              pointerAccess(instruction.operands().back(), definitions);
          if (!access.has_value() || access->base != data) continue;
          if (targetLoad != nullptr || access->index != consumer.induction) {
            targetLoad = nullptr;
            break;
          }
          targetLoad = &instruction;
          targetPointer = instruction.operands().back();
        }
        if (targetLoad == nullptr) continue;

        bool unsafe = false;
        for (std::size_t block = 0; block < function.blocks().size(); ++block) {
          for (const auto& instruction :
               function.blocks()[block].instructions()) {
            if (instruction.opcode() == "call") {
              for (std::size_t operand = 1;
                   operand < instruction.operands().size(); ++operand) {
                const auto root =
                    pointerRoot(instruction.operands()[operand], definitions);
                if (root == data && &instruction != call) {
                  unsafe = true;
                }
                if (root == rowAccess->base &&
                    afterCalls.find(block) != afterCalls.end()) {
                  unsafe = true;
                }
              }
            }
            if (instruction.opcode() != "load" &&
                instruction.opcode() != "store") {
              continue;
            }
            const auto root =
                pointerRoot(instruction.operands().back(), definitions);
            if (root == rowAccess->base) {
              if (instruction.opcode() == "store" &&
                  afterCalls.find(block) != afterCalls.end()) {
                unsafe = true;
              }
              continue;
            }
            if (root != data) continue;
            if (instruction.opcode() == "load" &&
                &instruction != targetLoad) {
              unsafe = true;
            }
            if (instruction.opcode() == "store" &&
                afterCalls.find(block) != afterCalls.end()) {
              unsafe = true;
            }
          }
        }
        for (std::size_t other = 0; other < module.functions().size();
             ++other) {
          if (other == functionIndex || other == kernel.functionIndex) {
            continue;
          }
          const auto otherDefinitions =
              definitionsOf(module.functions()[other]);
          for (const auto& block : module.functions()[other].blocks()) {
            for (const auto& instruction : block.instructions()) {
              if (instruction.opcode() != "load" &&
                  instruction.opcode() != "store") {
                continue;
              }
              const auto root = pointerRoot(instruction.operands().back(),
                                            otherDefinitions);
              if (root == data) unsafe = true;
            }
          }
        }
        if (unsafe) continue;
        return Candidate{functionIndex,
                         callLoop,
                         consumer,
                         count,
                         callLoop.bound,
                         data,
                         rowAccess->base,
                         targetPointer,
                         *dataCapacity,
                         *parameterCapacity};
      }
    }
  }
  return std::nullopt;
}

std::string blockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> used;
  for (const auto& block : function.blocks()) used.insert(block.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const auto name = stem + "." + std::to_string(suffix);
    if (used.find(name) == used.end()) return name;
  }
}

std::string valueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> used;
  for (const auto& parameter : function.parameters()) used.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) used.insert(instruction.result());
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const auto name = "%" + stem + "." + std::to_string(suffix);
    if (used.find(name) == used.end()) return name;
  }
}

Instruction replaceOperand(const Instruction& instruction,
                           const std::string& from,
                           const std::string& to) {
  auto operands = instruction.operands();
  for (auto& operand : operands) {
    if (operand == from) operand = to;
  }
  return Instruction{instruction.opcode(), std::move(operands),
                     instruction.result()};
}

void applyCandidate(Module& module, const Candidate& candidate) {
  auto& function = module.functions()[candidate.functionIndex];
  const auto indices = blockIndices(function);
  const auto validationEntry = blockName(function, "lazy.affine.validate.entry");
  const auto validationHeader = blockName(function, "lazy.affine.validate.header");
  const auto validationBody = blockName(function, "lazy.affine.validate.body");
  const auto validationLatch = blockName(function, "lazy.affine.validate.latch");
  const auto validationAccept = blockName(function, "lazy.affine.validate.accept");
  const auto validationFail = blockName(function, "lazy.affine.validate.fail");
  const auto passHeader = blockName(function, "lazy.affine.pass.header");
  const auto passPrepare = blockName(function, "lazy.affine.pass.prepare");
  const auto chainHeader = blockName(function, "lazy.affine.chain.header");
  const auto chainApply = blockName(function, "lazy.affine.chain.apply");
  const auto chainExit = blockName(function, "lazy.affine.chain.exit");
  const auto resume = blockName(function, "lazy.affine.resume");

  const auto v = [&](const std::string& stem) {
    return valueName(function, stem);
  };
  const auto modeSlot = v("lazy.affine.mode");
  const auto safeCount = v("lazy.affine.safe.count");
  const auto safeLength = v("lazy.affine.safe.length");
  const auto countCapacity = v("lazy.affine.count.capacity");
  const auto lengthCapacity = v("lazy.affine.length.capacity");
  const auto safePair = v("lazy.affine.safe.pair");
  const auto safeCapacity = v("lazy.affine.safe.capacity");
  const auto validationSafe = v("lazy.affine.validation.safe");
  const auto validationIndex = v("lazy.affine.validation.index");
  const auto validationNext = v("lazy.affine.validation.next");
  const auto validationMore = v("lazy.affine.validation.more");
  const auto validationPointer = v("lazy.affine.validation.pointer");
  const auto validationRow = v("lazy.affine.validation.row");
  const auto rowPositive = v("lazy.affine.row.positive");
  const auto rowWithin = v("lazy.affine.row.within");
  const auto rowSafe = v("lazy.affine.row.safe");
  const auto mode = v("lazy.affine.mode.value");
  const auto useLazy = v("lazy.affine.use");
  const auto passIndex = v("lazy.affine.pass.index");
  const auto passCounter = v("lazy.affine.pass.counter");
  const auto passMore = v("lazy.affine.pass.more");
  const auto previousPass = v("lazy.affine.pass.previous");
  const auto rowPointer = v("lazy.affine.row.pointer");
  const auto row = v("lazy.affine.row");
  const auto column = v("lazy.affine.column");
  const auto chainIndex = v("lazy.affine.chain.index");
  const auto cutoff = v("lazy.affine.chain.cutoff");
  const auto destinationRow = v("lazy.affine.destination.row");
  const auto destinationColumn = v("lazy.affine.destination.column");
  const auto sourceProduct = v("lazy.affine.source.product");
  const auto sourceIndex = v("lazy.affine.source.index");
  const auto rowValid = v("lazy.affine.destination.row.valid");
  const auto triangular = v("lazy.affine.triangular");
  const auto earlier = v("lazy.affine.earlier.writer");
  const auto destinationValid = v("lazy.affine.destination.valid");
  const auto follow = v("lazy.affine.follow");
  const auto resolvedIndex = v("lazy.affine.resolved.index");

  auto& entry = function.blocks().front().instructions();
  entry.insert(entry.end() - 1, Instruction{"alloca", {"int"}, modeSlot});

  auto& oldPreheader =
      function.blocks()[indices.at(candidate.calls.preheader)].instructions();
  oldPreheader.back() = replaceOperand(oldPreheader.back(),
                                       candidate.calls.header,
                                       validationEntry);

  auto& callHeader =
      function.blocks()[indices.at(candidate.calls.header)].instructions();
  for (auto& instruction : callHeader) {
    if (instruction.opcode() == "phi" &&
        instruction.result() == candidate.calls.induction) {
      instruction = replaceOperand(instruction, candidate.calls.preheader,
                                   validationFail);
    }
  }

  auto& consumerHeader =
      function.blocks()[indices.at(candidate.consumer.header)].instructions();
  for (auto& instruction : consumerHeader) {
    if (instruction.opcode() == "phi" &&
        instruction.result() == candidate.consumer.induction) {
      instruction =
          replaceOperand(instruction, candidate.consumer.body, resume);
    }
  }

  auto& oldBody =
      function.blocks()[indices.at(candidate.consumer.body)].instructions();
  // Keep the original call loop as a runtime fallback.  On the lazy path,
  // resolve each demanded array index backwards through the call sequence.
  // Within one copy pass, sourceIndex is also the original lexicographic
  // operation number.  Chasing only writers with sourceIndex < cutoff
  // therefore reproduces values written earlier in that same pass.
  std::vector<Instruction> body;
  body.push_back(Instruction{
      "phi",
      {"int", candidate.consumer.induction, candidate.consumer.body,
       passIndex, passHeader},
      resolvedIndex});
  for (const auto& instruction : oldBody) {
    if (instruction.result() == candidate.consumerPointer && isGep(instruction)) {
      body.push_back(Instruction{
          instruction.opcode(),
          {instruction.operands()[0], resolvedIndex},
          instruction.result()});
    } else {
      body.push_back(instruction);
    }
  }
  oldBody.clear();
  oldBody.push_back(Instruction{"load", {"int", modeSlot}, mode});
  oldBody.push_back(Instruction{"icmp.ne", {mode, "0"}, useLazy});
  oldBody.push_back(Instruction{"br", {useLazy, passHeader, resume}});

  BasicBlock validateEntry{validationEntry};
  validateEntry.addInstruction(
      Instruction{"icmp.sgt", {candidate.count, "0"}, safeCount});
  validateEntry.addInstruction(
      Instruction{"icmp.sge", {candidate.length, "0"}, safeLength});
  validateEntry.addInstruction(Instruction{
      "icmp.sle", {candidate.count, std::to_string(candidate.dataCapacity)},
      countCapacity});
  validateEntry.addInstruction(Instruction{
      "icmp.sle",
      {candidate.length, std::to_string(candidate.parameterCapacity)},
      lengthCapacity});
  validateEntry.addInstruction(Instruction{"and", {safeCount, safeLength}, safePair});
  validateEntry.addInstruction(
      Instruction{"and", {countCapacity, lengthCapacity}, safeCapacity});
  validateEntry.addInstruction(
      Instruction{"and", {safePair, safeCapacity}, validationSafe});
  validateEntry.addInstruction(
      Instruction{"br", {validationSafe, validationHeader, validationFail}});

  BasicBlock validateHeaderBlock{validationHeader};
  validateHeaderBlock.addInstruction(Instruction{
      "phi", {"int", "0", validationEntry, validationNext, validationLatch},
      validationIndex});
  validateHeaderBlock.addInstruction(Instruction{
      "icmp.slt", {validationIndex, candidate.length}, validationMore});
  validateHeaderBlock.addInstruction(
      Instruction{"br", {validationMore, validationBody, validationAccept}});

  BasicBlock validateBodyBlock{validationBody};
  validateBodyBlock.addInstruction(Instruction{
      "getelementptr", {candidate.parameters, validationIndex},
      validationPointer});
  validateBodyBlock.addInstruction(
      Instruction{"load", {"int", validationPointer}, validationRow});
  validateBodyBlock.addInstruction(
      Instruction{"icmp.sgt", {validationRow, "0"}, rowPositive});
  validateBodyBlock.addInstruction(
      Instruction{"icmp.sle", {validationRow, candidate.count}, rowWithin});
  validateBodyBlock.addInstruction(Instruction{"and", {rowPositive, rowWithin}, rowSafe});
  validateBodyBlock.addInstruction(
      Instruction{"br", {rowSafe, validationLatch, validationFail}});

  BasicBlock validateLatchBlock{validationLatch};
  validateLatchBlock.addInstruction(
      Instruction{"add", {validationIndex, "1"}, validationNext});
  validateLatchBlock.addInstruction(Instruction{"br", {validationHeader}});

  BasicBlock validateAcceptBlock{validationAccept};
  validateAcceptBlock.addInstruction(
      Instruction{"store", {"int", "1", modeSlot}});
  validateAcceptBlock.addInstruction(Instruction{"br", {candidate.calls.exit}});

  BasicBlock validateFailBlock{validationFail};
  validateFailBlock.addInstruction(
      Instruction{"store", {"int", "0", modeSlot}});
  validateFailBlock.addInstruction(Instruction{"br", {candidate.calls.header}});

  BasicBlock passHeaderBlock{passHeader};
  passHeaderBlock.addInstruction(Instruction{
      "phi",
      {"int", candidate.consumer.induction, candidate.consumer.body,
       chainIndex, chainExit},
      passIndex});
  passHeaderBlock.addInstruction(Instruction{
      "phi",
      {"int", candidate.length, candidate.consumer.body, previousPass,
       chainExit},
      passCounter});
  passHeaderBlock.addInstruction(
      Instruction{"icmp.sgt", {passCounter, "0"}, passMore});
  passHeaderBlock.addInstruction(
      Instruction{"br", {passMore, passPrepare, resume}});

  BasicBlock passPrepareBlock{passPrepare};
  passPrepareBlock.addInstruction(
      Instruction{"sub", {passCounter, "1"}, previousPass});
  passPrepareBlock.addInstruction(Instruction{
      "getelementptr", {candidate.parameters, previousPass}, rowPointer});
  passPrepareBlock.addInstruction(
      Instruction{"load", {"int", rowPointer}, row});
  passPrepareBlock.addInstruction(
      Instruction{"sdiv", {candidate.count, row}, column});
  passPrepareBlock.addInstruction(Instruction{"br", {chainHeader}});

  BasicBlock chainHeaderBlock{chainHeader};
  chainHeaderBlock.addInstruction(Instruction{
      "phi", {"int", passIndex, passPrepare, sourceIndex, chainApply},
      chainIndex});
  chainHeaderBlock.addInstruction(Instruction{
      "phi", {"int", candidate.count, passPrepare, sourceIndex, chainApply},
      cutoff});
  chainHeaderBlock.addInstruction(
      Instruction{"sdiv", {chainIndex, column}, destinationRow});
  chainHeaderBlock.addInstruction(
      Instruction{"srem", {chainIndex, column}, destinationColumn});
  chainHeaderBlock.addInstruction(
      Instruction{"mul", {destinationColumn, row}, sourceProduct});
  chainHeaderBlock.addInstruction(
      Instruction{"add", {sourceProduct, destinationRow}, sourceIndex});
  chainHeaderBlock.addInstruction(
      Instruction{"icmp.slt", {destinationRow, row}, rowValid});
  chainHeaderBlock.addInstruction(
      Instruction{"icmp.sle", {destinationRow, destinationColumn}, triangular});
  chainHeaderBlock.addInstruction(
      Instruction{"icmp.slt", {sourceIndex, cutoff}, earlier});
  chainHeaderBlock.addInstruction(
      Instruction{"and", {rowValid, triangular}, destinationValid});
  chainHeaderBlock.addInstruction(
      Instruction{"and", {destinationValid, earlier}, follow});
  chainHeaderBlock.addInstruction(
      Instruction{"br", {follow, chainApply, chainExit}});

  BasicBlock chainApplyBlock{chainApply};
  chainApplyBlock.addInstruction(Instruction{"br", {chainHeader}});

  BasicBlock chainExitBlock{chainExit};
  chainExitBlock.addInstruction(Instruction{"br", {passHeader}});

  BasicBlock resumeBlock{resume};
  for (auto& instruction : body) {
    resumeBlock.addInstruction(std::move(instruction));
  }

  function.addBlock(std::move(validateEntry));
  function.addBlock(std::move(validateHeaderBlock));
  function.addBlock(std::move(validateBodyBlock));
  function.addBlock(std::move(validateLatchBlock));
  function.addBlock(std::move(validateAcceptBlock));
  function.addBlock(std::move(validateFailBlock));
  function.addBlock(std::move(passHeaderBlock));
  function.addBlock(std::move(passPrepareBlock));
  function.addBlock(std::move(chainHeaderBlock));
  function.addBlock(std::move(chainApplyBlock));
  function.addBlock(std::move(chainExitBlock));
  function.addBlock(std::move(resumeBlock));
}

}  // namespace

void LazyAffineCopyResolutionPass::run(Module& module) {
  if (std::getenv("COMPILER_DISABLE_LAZY_AFFINE_COPY_RESOLUTION") != nullptr) {
    return;
  }
  std::size_t transformed = 0;
  for (std::size_t functionIndex = 0;
       functionIndex < module.functions().size(); ++functionIndex) {
    const auto kernel =
        analyzeKernel(module.functions()[functionIndex], functionIndex);
    if (!kernel.has_value()) continue;
    const auto candidate = analyzeCaller(module, *kernel);
    if (!candidate.has_value()) continue;
    applyCandidate(module, *candidate);
    ++transformed;
    break;
  }
  if (std::getenv("COMPILER_LAZY_AFFINE_COPY_STATS") != nullptr) {
    std::cerr << "[lazy-affine-copy] transformed=" << transformed << '\n';
  }
}

}  // namespace compiler::midend::pass
