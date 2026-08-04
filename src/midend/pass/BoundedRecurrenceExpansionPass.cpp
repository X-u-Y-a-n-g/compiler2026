#include "../../../include/compiler/midend/pass/BoundedRecurrenceExpansionPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
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
  std::size_t index = 0;
};

struct Candidate {
  std::size_t header = 0;
  std::size_t body = 0;
  std::size_t exit = 0;
  std::size_t preheader = 0;
  std::string headerName;
  std::string bodyName;
  std::string exitName;
  std::string preheaderName;
  std::string statePhi;
  std::string initialState;
  std::string stateNext;
  std::string counterSlot;
  std::string boundSlot;
  std::string boundValue;
  long long initialCounter = 0;
  std::vector<long long> bounds;
  std::vector<Instruction> recurrence;
};

bool isGep(const Instruction& instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "getelementptr.linear" ||
         instruction.opcode() == "gep" ||
         instruction.opcode() == "gep.linear";
}

std::unordered_map<std::string, Definition> definitionsOf(
    const Function& function) {
  std::unordered_map<std::string, Definition> definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].hasResult()) {
        definitions.emplace(instructions[index].result(),
                            Definition{&instructions[index], block, index});
      }
    }
  }
  return definitions;
}

std::unordered_map<std::string, std::size_t> blockIndices(
    const Function& function) {
  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    indices.emplace(function.blocks()[index].name(), index);
  }
  return indices;
}

std::string pointerRoot(
    const std::string& pointer,
    const std::unordered_map<std::string, Definition>& definitions) {
  std::string root = pointer;
  std::unordered_set<std::string> visiting;
  while (!root.empty() && root.front() == '%') {
    if (!visiting.insert(root).second) return {};
    const auto found = definitions.find(root);
    if (found == definitions.end()) break;
    const auto& instruction = *found->second.instruction;
    if ((!isGep(instruction) && instruction.opcode() != "cast") ||
        instruction.operands().empty()) {
      break;
    }
    root = instruction.operands().front();
  }
  return root;
}

struct ArrayConstants {
  std::unordered_map<std::string, std::vector<long long>> values;
};

ArrayConstants collectArrayConstants(
    const Function& function,
    const std::unordered_map<std::string, Definition>& definitions) {
  std::unordered_set<std::string> arrays;
  for (const auto& [value, definition] : definitions) {
    if (definition.instruction->opcode() == "alloca" &&
        definition.instruction->operands().size() == 1 &&
        definition.instruction->operands()[0].find('[') != std::string::npos) {
      arrays.insert(value);
    }
  }
  std::unordered_set<std::string> escaped;
  std::unordered_map<std::string, std::set<long long>> constants;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t operand = 0; operand < instruction.operands().size();
           ++operand) {
        const auto root = pointerRoot(instruction.operands()[operand], definitions);
        if (arrays.find(root) == arrays.end()) continue;
        const bool gepBase = isGep(instruction) && operand == 0;
        const bool loadAddress = instruction.opcode() == "load" && operand == 1;
        const bool storeAddress = instruction.opcode() == "store" && operand == 2;
        if (!gepBase && !loadAddress && !storeAddress) escaped.insert(root);
      }
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3) {
        continue;
      }
      const auto root =
          pointerRoot(instruction.operands().back(), definitions);
      if (arrays.find(root) == arrays.end()) continue;
      long long value = 0;
      if (!detail::parseIntegerLiteral(instruction.operands()[1], value)) {
        escaped.insert(root);
        continue;
      }
      constants[root].insert(value);
    }
  }
  ArrayConstants result;
  for (const auto& root : arrays) {
    if (escaped.find(root) != escaped.end()) continue;
    const auto found = constants.find(root);
    if (found == constants.end() || found->second.empty()) continue;
    result.values.emplace(root, std::vector<long long>(found->second.begin(),
                                                       found->second.end()));
  }
  return result;
}

bool isSafeRecurrenceInstruction(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  if (opcode == "add" || opcode == "sub" || opcode == "mul" ||
      opcode == "and" || opcode == "or" || opcode == "xor" ||
      opcode == "shl" || opcode == "ashr" || opcode == "lshr" ||
      opcode == "select" || opcode.rfind("icmp.", 0) == 0) {
    return true;
  }
  if ((opcode == "srem" || opcode == "mod") &&
      instruction.operands().size() == 2) {
    long long divisor = 0;
    return detail::parseIntegerLiteral(instruction.operands()[1], divisor) &&
           divisor != 0 && divisor != -1;
  }
  return false;
}

bool collectRecurrenceSlice(
    const std::string& value, const Candidate& candidate,
    const std::unordered_map<std::string, Definition>& definitions,
    std::unordered_set<std::string>& visiting,
    std::unordered_set<std::string>& selected) {
  if (value == candidate.statePhi) return true;
  const auto definition = definitions.find(value);
  if (definition == definitions.end()) {
    return true;
  }
  if (definition->second.block == candidate.header) return false;
  if (definition->second.block != candidate.body) return true;
  if (!visiting.insert(value).second ||
      !isSafeRecurrenceInstruction(*definition->second.instruction)) {
    return false;
  }
  for (const auto& operand : definition->second.instruction->operands()) {
    if (!collectRecurrenceSlice(operand, candidate, definitions, visiting,
                                selected)) {
      visiting.erase(value);
      return false;
    }
  }
  visiting.erase(value);
  selected.insert(value);
  return true;
}

bool slotUsesArePrivate(const Function& function, const Candidate& candidate) {
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      for (std::size_t operand = 0; operand < instruction.operands().size();
           ++operand) {
        const auto& value = instruction.operands()[operand];
        if (value != candidate.counterSlot && value != candidate.boundSlot) continue;
        const bool address =
            (instruction.opcode() == "load" && operand == 1) ||
            (instruction.opcode() == "store" && operand == 2);
        if (!address) return false;
        if (value == candidate.counterSlot && block != candidate.preheader &&
            block != candidate.header && block != candidate.body) {
          return false;
        }
        if (value == candidate.boundSlot && block != candidate.preheader &&
            block != candidate.header) {
          return false;
        }
      }
    }
  }
  return true;
}

std::optional<Candidate> findCandidate(const Function& function) {
  const auto definitions = definitionsOf(function);
  const auto indices = blockIndices(function);
  const auto arrays = collectArrayConstants(function, definitions);
  for (std::size_t headerIndex = 0; headerIndex < function.blocks().size();
       ++headerIndex) {
    const auto& header = function.blocks()[headerIndex];
    const auto* branch = header.terminator();
    if (branch == nullptr || branch->opcode() != "br" ||
        branch->operands().size() != 3) {
      continue;
    }
    const auto comparison = definitions.find(branch->operands()[0]);
    if (comparison == definitions.end() ||
        comparison->second.block != headerIndex ||
        comparison->second.instruction->opcode() != "icmp.slt" ||
        comparison->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto counterLoad =
        definitions.find(comparison->second.instruction->operands()[0]);
    const auto boundLoad =
        definitions.find(comparison->second.instruction->operands()[1]);
    if (counterLoad == definitions.end() || boundLoad == definitions.end() ||
        counterLoad->second.block != headerIndex ||
        boundLoad->second.block != headerIndex ||
        counterLoad->second.instruction->opcode() != "load" ||
        boundLoad->second.instruction->opcode() != "load" ||
        counterLoad->second.instruction->operands().size() != 2 ||
        boundLoad->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto body = indices.find(branch->operands()[1]);
    const auto exit = indices.find(branch->operands()[2]);
    if (body == indices.end() || exit == indices.end()) continue;
    const auto* bodyBranch = function.blocks()[body->second].terminator();
    if (bodyBranch == nullptr || bodyBranch->opcode() != "br" ||
        bodyBranch->operands().size() != 1 ||
        bodyBranch->operands()[0] != header.name()) {
      continue;
    }
    if (function.blocks()[exit->second].instructions().empty() ||
        function.blocks()[exit->second].instructions().front().opcode() == "phi") {
      continue;
    }

    const Instruction* statePhi = nullptr;
    for (const auto& instruction : header.instructions()) {
      if (instruction.opcode() != "phi") continue;
      if (statePhi != nullptr || instruction.operands().size() != 5 ||
          instruction.operands()[0] != "int") {
        statePhi = nullptr;
        break;
      }
      statePhi = &instruction;
    }
    if (statePhi == nullptr) continue;

    Candidate candidate;
    candidate.header = headerIndex;
    candidate.body = body->second;
    candidate.exit = exit->second;
    candidate.headerName = header.name();
    candidate.bodyName = function.blocks()[body->second].name();
    candidate.exitName = function.blocks()[exit->second].name();
    candidate.statePhi = statePhi->result();
    candidate.counterSlot = counterLoad->second.instruction->operands().back();
    candidate.boundSlot = boundLoad->second.instruction->operands().back();
    if (candidate.counterSlot == candidate.boundSlot) continue;
    bool headerIsCanonical = true;
    for (const auto& instruction : header.instructions()) {
      if (&instruction == statePhi ||
          &instruction == counterLoad->second.instruction ||
          &instruction == boundLoad->second.instruction ||
          &instruction == comparison->second.instruction ||
          &instruction == branch) {
        continue;
      }
      headerIsCanonical = false;
      break;
    }
    if (!headerIsCanonical) continue;
    for (std::size_t incoming = 1; incoming + 1 < statePhi->operands().size();
         incoming += 2) {
      if (statePhi->operands()[incoming + 1] == candidate.bodyName) {
        candidate.stateNext = statePhi->operands()[incoming];
      } else {
        candidate.initialState = statePhi->operands()[incoming];
        candidate.preheaderName = statePhi->operands()[incoming + 1];
      }
    }
    const auto preheader = indices.find(candidate.preheaderName);
    if (candidate.stateNext.empty() || candidate.initialState.empty() ||
        preheader == indices.end()) {
      continue;
    }
    candidate.preheader = preheader->second;
    const auto* preheaderBranch =
        function.blocks()[candidate.preheader].terminator();
    if (preheaderBranch == nullptr || preheaderBranch->opcode() != "br" ||
        preheaderBranch->operands().size() != 1 ||
        preheaderBranch->operands()[0] != candidate.headerName) {
      continue;
    }

    bool counterInitialized = false;
    bool boundInitialized = false;
    for (const auto& instruction :
         function.blocks()[candidate.preheader].instructions()) {
      if (instruction.opcode() != "store" || instruction.operands().size() != 3)
        continue;
      if (instruction.operands().back() == candidate.counterSlot) {
        counterInitialized = detail::parseIntegerLiteral(
            instruction.operands()[1], candidate.initialCounter);
      }
      if (instruction.operands().back() == candidate.boundSlot) {
        candidate.boundValue = instruction.operands()[1];
        boundInitialized = true;
      }
    }
    if (!counterInitialized || !boundInitialized) continue;
    const auto boundSource = definitions.find(candidate.boundValue);
    if (boundSource == definitions.end() ||
        boundSource->second.instruction->opcode() != "load" ||
        boundSource->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto arrayRoot = pointerRoot(
        boundSource->second.instruction->operands().back(), definitions);
    const auto constants = arrays.values.find(arrayRoot);
    if (constants == arrays.values.end()) continue;
    for (const auto bound : constants->second) {
      const auto iterations = bound - candidate.initialCounter;
      if (iterations > 0 && iterations <= 8) candidate.bounds.push_back(bound);
    }
    std::sort(candidate.bounds.begin(), candidate.bounds.end(),
              [&](long long lhs, long long rhs) {
                return lhs - candidate.initialCounter >
                       rhs - candidate.initialCounter;
              });
    if (candidate.bounds.size() > 8) candidate.bounds.resize(8);
    if (candidate.bounds.empty() || !slotUsesArePrivate(function, candidate))
      continue;

    const Instruction* counterLoadInBody = nullptr;
    const Instruction* counterIncrement = nullptr;
    const Instruction* counterStore = nullptr;
    for (const auto& instruction : function.blocks()[candidate.body].instructions()) {
      if (instruction.opcode() != "store") continue;
      if (counterStore != nullptr || instruction.operands().size() != 3 ||
          instruction.operands().back() != candidate.counterSlot) {
        counterStore = nullptr;
        break;
      }
      const auto increment = definitions.find(instruction.operands()[1]);
      if (increment == definitions.end() ||
          increment->second.block != candidate.body ||
          increment->second.instruction->opcode() != "add" ||
          increment->second.instruction->operands().size() != 2) {
        counterStore = nullptr;
        break;
      }
      const auto& operands = increment->second.instruction->operands();
      const auto lhsLoad = definitions.find(operands[0]);
      const auto rhsLoad = definitions.find(operands[1]);
      if (lhsLoad != definitions.end() &&
          lhsLoad->second.block == candidate.body &&
          lhsLoad->second.instruction->opcode() == "load" &&
          lhsLoad->second.instruction->operands().size() == 2 &&
          lhsLoad->second.instruction->operands().back() ==
              candidate.counterSlot &&
          detail::isOne(operands[1])) {
        counterLoadInBody = lhsLoad->second.instruction;
      } else if (rhsLoad != definitions.end() &&
                 rhsLoad->second.block == candidate.body &&
                 rhsLoad->second.instruction->opcode() == "load" &&
                 rhsLoad->second.instruction->operands().size() == 2 &&
                 rhsLoad->second.instruction->operands().back() ==
                     candidate.counterSlot &&
                 detail::isOne(operands[0])) {
        counterLoadInBody = rhsLoad->second.instruction;
      } else {
        counterStore = nullptr;
        break;
      }
      counterIncrement = increment->second.instruction;
      counterStore = &instruction;
    }
    if (counterLoadInBody == nullptr || counterIncrement == nullptr ||
        counterStore == nullptr) {
      continue;
    }

    std::unordered_set<std::string> selected;
    std::unordered_set<std::string> visiting;
    if (!collectRecurrenceSlice(candidate.stateNext, candidate, definitions,
                                visiting, selected)) {
      continue;
    }
    for (std::size_t index = 0;
         index < function.blocks()[candidate.body].instructions().size(); ++index) {
      const auto& instruction =
          function.blocks()[candidate.body].instructions()[index];
      if (selected.find(instruction.result()) != selected.end()) {
        candidate.recurrence.push_back(instruction);
        continue;
      }
      if (&instruction == bodyBranch || &instruction == counterLoadInBody ||
          &instruction == counterIncrement || &instruction == counterStore) {
        continue;
      }
      candidate.recurrence.clear();
      break;
    }
    if (candidate.recurrence.empty()) continue;
    return candidate;
  }
  return std::nullopt;
}

std::unordered_set<std::string> valueNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  return names;
}

std::unordered_set<std::string> blockNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) names.insert(block.name());
  return names;
}

std::string freshValue(std::unordered_set<std::string>& names,
                       const std::string& stem, std::size_t& counter) {
  for (;;) {
    const auto candidate = "%" + stem + "." + std::to_string(counter++);
    if (names.insert(candidate).second) return candidate;
  }
}

std::string freshBlock(std::unordered_set<std::string>& names,
                       const std::string& stem, std::size_t& counter) {
  for (;;) {
    const auto candidate = stem + "." + std::to_string(counter++);
    if (names.insert(candidate).second) return candidate;
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

void applyCandidate(Function& function, const Candidate& candidate) {
  auto values = valueNames(function);
  auto blocks = blockNames(function);
  std::size_t valueCounter = 0;
  std::size_t blockCounter = 0;
  std::vector<long long> bounds = candidate.bounds;
  std::sort(bounds.begin(), bounds.end(), std::greater<long long>());
  const auto fallback =
      freshBlock(blocks, "bounded.recurrence.fallback", blockCounter);
  std::vector<std::string> dispatches;
  dispatches.reserve(bounds.size());
  for (std::size_t index = 0; index < bounds.size(); ++index) {
    dispatches.push_back(
        freshBlock(blocks, "bounded.recurrence.dispatch", blockCounter));
  }
  const auto maximumIterations = static_cast<std::size_t>(
      bounds.front() - candidate.initialCounter);
  std::vector<std::string> steps(maximumIterations + 1);
  for (std::size_t step = 1; step <= maximumIterations; ++step) {
    steps[step] = freshBlock(blocks, "bounded.recurrence.step", blockCounter);
  }

  auto& preheader = function.blocks()[candidate.preheader].instructions();
  preheader.back() = replaceOperand(preheader.back(), candidate.headerName,
                                    dispatches.front());
  auto& header = function.blocks()[candidate.header].instructions();
  for (auto& instruction : header) {
    if (instruction.opcode() == "phi" &&
        instruction.result() == candidate.statePhi) {
      instruction = replaceOperand(instruction, candidate.preheaderName, fallback);
    }
  }

  const auto finalState =
      freshValue(values, "bounded.recurrence.final", valueCounter);
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    if (block == candidate.header || block == candidate.body) continue;
    auto& instructions = function.blocks()[block].instructions();
    for (auto& instruction : instructions) {
      instruction = replaceOperand(instruction, candidate.statePhi, finalState);
    }
  }
  std::unordered_map<std::size_t, std::string> directDispatch;
  for (std::size_t index = 0; index < bounds.size(); ++index) {
    const auto iterations =
        static_cast<std::size_t>(bounds[index] - candidate.initialCounter);
    directDispatch[iterations] = dispatches[index];
    BasicBlock dispatch{dispatches[index]};
    const auto matches =
        freshValue(values, "bounded.recurrence.matches", valueCounter);
    dispatch.addInstruction(Instruction{
        "icmp.eq", {candidate.boundValue, std::to_string(bounds[index])}, matches});
    dispatch.addInstruction(Instruction{
        "br", {matches, steps[iterations],
               index + 1 < dispatches.size() ? dispatches[index + 1] : fallback}});
    function.addBlock(std::move(dispatch));
  }
  BasicBlock fallbackBlock{fallback};
  fallbackBlock.addInstruction(Instruction{"br", {candidate.headerName}});
  function.addBlock(std::move(fallbackBlock));

  std::vector<std::string> stepOutputs(maximumIterations + 1);
  for (std::size_t step = maximumIterations; step > 0; --step) {
    BasicBlock block{steps[step]};
    std::vector<std::string> incoming;
    const auto direct = directDispatch.find(step);
    if (direct != directDispatch.end()) {
      incoming.insert(incoming.end(),
                      {candidate.initialState, direct->second});
    }
    if (step < maximumIterations) {
      incoming.insert(incoming.end(), {stepOutputs[step + 1], steps[step + 1]});
    }
    std::string state = incoming.front();
    if (incoming.size() > 2) {
      state = freshValue(values, "bounded.recurrence.state", valueCounter);
      std::vector<std::string> operands{"int"};
      operands.insert(operands.end(), incoming.begin(), incoming.end());
      block.addInstruction(Instruction{"phi", std::move(operands), state});
    }
    std::unordered_map<std::string, std::string> replacements{
        {candidate.statePhi, state}};
    for (const auto& instruction : candidate.recurrence) {
      auto operands = instruction.operands();
      for (auto& operand : operands) {
        const auto replacement = replacements.find(operand);
        if (replacement != replacements.end()) operand = replacement->second;
      }
      const auto result =
          freshValue(values, "bounded.recurrence.value", valueCounter);
      replacements[instruction.result()] = result;
      block.addInstruction(
          Instruction{instruction.opcode(), std::move(operands), result});
    }
    stepOutputs[step] = replacements.at(candidate.stateNext);
    block.addInstruction(Instruction{
        "br", {step == 1 ? candidate.exitName : steps[step - 1]}});
    function.addBlock(std::move(block));
  }
  auto& exitInstructions = function.blocks()[candidate.exit].instructions();
  exitInstructions.insert(
      exitInstructions.begin(),
      Instruction{"phi",
                  {"int", candidate.statePhi, candidate.headerName,
                   stepOutputs[1], steps[1]},
                  finalState});
}

}  // namespace

void BoundedRecurrenceExpansionPass::run(Module& module) {
  if (std::getenv("COMPILER_DISABLE_BOUNDED_RECURRENCE_EXPANSION") != nullptr)
    return;
  std::size_t transformed = 0;
  for (auto& function : module.functions()) {
    const auto candidate = findCandidate(function);
    if (!candidate.has_value()) continue;
    applyCandidate(function, *candidate);
    ++transformed;
  }
  if (std::getenv("COMPILER_BOUNDED_RECURRENCE_STATS") != nullptr) {
    std::cerr << "[bounded-recurrence] transformed=" << transformed << '\n';
  }
}

}  // namespace compiler::midend::pass
