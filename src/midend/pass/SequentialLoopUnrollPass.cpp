#include "../../../include/compiler/midend/pass/SequentialLoopUnrollPass.h"

#include <algorithm>
#include <iterator>
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

constexpr std::size_t kMaximumBodyInstructions = 24;

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> predecessors;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string exit;
  std::string inductionSlot;
  std::string inductionValue;
  std::string bound;
};

struct PhiCandidate {
  std::string header;
  std::string body;
  std::string exit;
  std::string inductionPhi;
  std::string nextInduction;
  std::string bound;
};

using DefinitionMap = std::unordered_map<std::string, const Instruction*>;

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (const auto& block : function.blocks()) {
    const auto* terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }
    const auto& operands = terminator->operands();
    const std::size_t firstTarget = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = firstTarget; index < operands.size(); ++index) {
      const auto target = cfg.index.find(operands[index]);
      if (target == cfg.index.end()) {
        continue;
      }
      auto& predecessors = cfg.predecessors[target->second];
      if (std::find(predecessors.begin(), predecessors.end(), block.name()) ==
          predecessors.end()) {
        predecessors.push_back(block.name());
      }
    }
  }
  return cfg;
}

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(), &instruction);
      }
    }
  }
  return definitions;
}

const BasicBlock* findBlock(const Function& function, const CFG& cfg,
                            const std::string& name) {
  const auto found = cfg.index.find(name);
  return found == cfg.index.end() ? nullptr
                                 : &function.blocks()[found->second];
}

BasicBlock* findBlock(Function& function, const std::string& name) {
  for (auto& block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

bool isLoadFrom(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "load" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         instruction.operands()[1] == slot;
}

bool isStoreTo(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "store" &&
         instruction.operands().size() == 3 &&
         instruction.operands()[2] == slot;
}

bool hasLeadingPhi(const BasicBlock& block) {
  return !block.instructions().empty() &&
         block.instructions().front().opcode() == "phi";
}

bool isIntAlloca(const std::string& value,
                 const DefinitionMap& definitions) {
  const auto found = definitions.find(value);
  return found != definitions.end() &&
         found->second->opcode() == "alloca" &&
         found->second->operands() == std::vector<std::string>{"int"};
}

bool isNonEscapingScalarSlot(const Function& function,
                             const std::string& slot) {
  bool hasLoad = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand != slot) {
          continue;
        }
        if (isLoadFrom(instruction, slot)) {
          hasLoad = true;
          continue;
        }
        if (isStoreTo(instruction, slot)) {
          continue;
        }
        return false;
      }
    }
  }
  return hasLoad;
}

bool isNonEscapingReadOnlySlot(const Function& function,
                               const std::string& slot,
                               const std::string& loopBody) {
  bool hasLoad = false;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand != slot) {
          continue;
        }
        if (isLoadFrom(instruction, slot)) {
          hasLoad = true;
          continue;
        }
        if (isStoreTo(instruction, slot) && block.name() != loopBody) {
          continue;
        }
        return false;
      }
    }
  }
  return hasLoad;
}

bool boundIsStable(const Function& function, const Candidate& candidate,
                   const DefinitionMap& definitions) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(candidate.bound, literal)) {
    return literal >= -2147483648LL && literal <= 2147483647LL;
  }

  const auto definition = definitions.find(candidate.bound);
  if (definition == definitions.end()) {
    return std::any_of(function.parameters().begin(),
                       function.parameters().end(), [&](const auto& parameter) {
                         return parameter.name == candidate.bound &&
                                parameter.type == "int";
                       });
  }
  if (definition->second->opcode() == "load") {
    if (definition->second->operands().size() != 2 ||
        definition->second->operands()[0] != "int") {
      return false;
    }
    const auto& slot = definition->second->operands()[1];
    return isIntAlloca(slot, definitions) &&
           isNonEscapingReadOnlySlot(function, slot, candidate.body);
  }

  // An SSA value defined outside the loop cannot change between iterations.
  for (const auto& block : function.blocks()) {
    if (block.name() != candidate.header &&
        block.name() != candidate.body) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      if (&instruction == definition->second) {
        return false;
      }
    }
  }
  return true;
}

bool bodyResultsDoNotEscape(const Function& function,
                            const BasicBlock& body) {
  std::unordered_set<std::string> results;
  for (const auto& instruction : body.instructions()) {
    if (instruction.hasResult()) {
      results.insert(instruction.result());
    }
  }
  for (const auto& block : function.blocks()) {
    if (block.name() == body.name()) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (results.find(operand) != results.end()) {
          return false;
        }
      }
    }
  }
  return true;
}

bool valueIsLocalToBlock(const Function& function, const std::string& value,
                         const std::string& blockName) {
  for (const auto& block : function.blocks()) {
    if (block.name() == blockName) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    value) != instruction.operands().end()) {
        return false;
      }
    }
  }
  return true;
}

bool hasProfitableStraightLineBody(const BasicBlock& body) {
  if (body.instructions().size() < 2 ||
      body.instructions().size() > kMaximumBodyInstructions + 1) {
    return false;
  }
  bool hasExpensiveOperation = false;
  for (std::size_t index = 0; index + 1 < body.instructions().size(); ++index) {
    const auto& instruction = body.instructions()[index];
    if (instruction.opcode() == "call" || instruction.opcode() == "phi" ||
        instruction.opcode() == "alloca" || instruction.isTerminator()) {
      return false;
    }
    const auto& opcode = instruction.opcode();
    hasExpensiveOperation =
        hasExpensiveOperation || opcode == "load" || opcode == "store" ||
        opcode == "mul" || opcode == "div" || opcode == "mod" ||
        opcode == "sdiv" || opcode == "srem";
  }
  return hasExpensiveOperation;
}

bool hasUnitInductionUpdate(const BasicBlock& body,
                            const std::string& slot,
                            const DefinitionMap& definitions) {
  std::size_t stores = 0;
  bool foundUnitUpdate = false;
  for (const auto& instruction : body.instructions()) {
    if (!isStoreTo(instruction, slot)) {
      continue;
    }
    ++stores;
    const auto add = definitions.find(instruction.operands()[1]);
    if (add == definitions.end() || add->second->opcode() != "add" ||
        add->second->operands().size() != 2) {
      continue;
    }
    const auto& operands = add->second->operands();
    const auto isOne = [](const std::string& operand) {
      long long value = 0;
      return detail::parseIntegerLiteral(operand, value) && value == 1;
    };
    const auto loadsSlot = [&](std::size_t valueIndex,
                               std::size_t unitIndex) {
      if (!isOne(operands[unitIndex])) {
        return false;
      }
      const auto load = definitions.find(operands[valueIndex]);
      return load != definitions.end() &&
             isLoadFrom(*load->second, slot);
    };
    foundUnitUpdate = loadsSlot(0, 1) || loadsSlot(1, 0);
  }
  return stores == 1 && foundUnitUpdate;
}

bool headerOnlyChecksLoopCondition(const BasicBlock& header,
                                   const Candidate& candidate,
                                   const DefinitionMap& definitions) {
  for (std::size_t index = 0; index + 1 < header.instructions().size();
       ++index) {
    const auto& instruction = header.instructions()[index];
    if (isLoadFrom(instruction, candidate.inductionSlot)) {
      continue;
    }
    if (instruction.hasResult() &&
        instruction.result() == candidate.bound &&
        instruction.opcode() == "load") {
      continue;
    }
    if (instruction.opcode() == "icmp.slt" &&
        instruction.operands() ==
            std::vector<std::string>{candidate.inductionValue,
                                     candidate.bound}) {
      continue;
    }
    return false;
  }

  const auto condition =
      definitions.find(header.instructions().back().operands()[0]);
  return condition != definitions.end() &&
         condition->second->opcode() == "icmp.slt";
}

std::vector<Candidate> findSlotCandidates(const Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  std::vector<Candidate> candidates;

  for (const auto& header : function.blocks()) {
    const auto* terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto condition = definitions.find(terminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second->opcode() != "icmp.slt" ||
        condition->second->operands().size() != 2) {
      continue;
    }
    const auto inductionLoad =
        definitions.find(condition->second->operands()[0]);
    if (inductionLoad == definitions.end() ||
        inductionLoad->second->opcode() != "load" ||
        inductionLoad->second->operands().size() != 2 ||
        inductionLoad->second->operands()[0] != "int") {
      continue;
    }

    const auto bodyName = terminator->operands()[1];
    const auto exitName = terminator->operands()[2];
    const auto* body = findBlock(function, cfg, bodyName);
    const auto* exit = findBlock(function, cfg, exitName);
    const auto headerIndex = cfg.index.find(header.name());
    const auto bodyIndex = cfg.index.find(bodyName);
    if (body == nullptr || exit == nullptr || hasLeadingPhi(*exit) ||
        headerIndex == cfg.index.end() ||
        bodyIndex == cfg.index.end() ||
        cfg.predecessors[headerIndex->second].size() != 2 ||
        cfg.predecessors[bodyIndex->second] !=
            std::vector<std::string>{header.name()} ||
        body->terminator() == nullptr ||
        body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    Candidate candidate{header.name(),
                        bodyName,
                        exitName,
                        inductionLoad->second->operands()[1],
                        inductionLoad->second->result(),
                        condition->second->operands()[1]};
    if (!isIntAlloca(candidate.inductionSlot, definitions) ||
        !isNonEscapingScalarSlot(function, candidate.inductionSlot) ||
        !hasProfitableStraightLineBody(*body) ||
        !hasUnitInductionUpdate(*body, candidate.inductionSlot, definitions) ||
        !bodyResultsDoNotEscape(function, *body) ||
        !valueIsLocalToBlock(function, candidate.inductionValue,
                             candidate.header) ||
        !headerOnlyChecksLoopCondition(header, candidate, definitions) ||
        !boundIsStable(function, candidate, definitions)) {
      continue;
    }
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

bool bodyResultsDoNotEscapeExcept(const Function& function,
                                  const BasicBlock& body,
                                  const PhiCandidate& candidate) {
  std::unordered_set<std::string> results;
  for (const auto& instruction : body.instructions()) {
    if (instruction.hasResult() &&
        instruction.result() != candidate.nextInduction) {
      results.insert(instruction.result());
    }
  }
  for (const auto& block : function.blocks()) {
    if (block.name() == body.name()) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand == candidate.nextInduction &&
            !(block.name() == candidate.header &&
              instruction.opcode() == "phi" &&
              instruction.result() == candidate.inductionPhi)) {
          return false;
        }
        if (results.find(operand) != results.end()) {
          return false;
        }
      }
    }
  }
  return true;
}

bool phiBoundIsStable(const Function& function,
                      const PhiCandidate& candidate,
                      const DefinitionMap& definitions) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(candidate.bound, literal)) {
    return literal >= -2147483648LL && literal <= 2147483647LL;
  }
  const auto definition = definitions.find(candidate.bound);
  if (definition == definitions.end()) {
    return std::any_of(function.parameters().begin(),
                       function.parameters().end(), [&](const auto& parameter) {
                         return parameter.name == candidate.bound &&
                                parameter.type == "int";
                       });
  }
  for (const auto& block : function.blocks()) {
    if (block.name() != candidate.header &&
        block.name() != candidate.body) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      if (&instruction == definition->second) {
        return false;
      }
    }
  }
  return true;
}

bool inductionPhiDoesNotEscapeLoop(const Function& function,
                                   const PhiCandidate& candidate) {
  for (const auto& block : function.blocks()) {
    if (block.name() == candidate.header ||
        block.name() == candidate.body) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    candidate.inductionPhi) !=
          instruction.operands().end()) {
        return false;
      }
    }
  }
  return true;
}

bool hasUnitPhiUpdate(const BasicBlock& body,
                      const PhiCandidate& candidate,
                      const DefinitionMap& definitions) {
  const auto update = definitions.find(candidate.nextInduction);
  if (update == definitions.end() || update->second->opcode() != "add" ||
      update->second->operands().size() != 2) {
    return false;
  }
  bool definitionIsInBody = false;
  for (const auto& instruction : body.instructions()) {
    if (&instruction == update->second) {
      definitionIsInBody = true;
      break;
    }
  }
  if (!definitionIsInBody) {
    return false;
  }
  const auto& operands = update->second->operands();
  long long left = 0;
  long long right = 0;
  return (operands[0] == candidate.inductionPhi &&
          detail::parseIntegerLiteral(operands[1], right) && right == 1) ||
         (operands[1] == candidate.inductionPhi &&
          detail::parseIntegerLiteral(operands[0], left) && left == 1);
}

std::vector<PhiCandidate> findPhiCandidates(const Function& function) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);
  std::vector<PhiCandidate> candidates;

  for (const auto& header : function.blocks()) {
    const auto* terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto condition = definitions.find(terminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second->opcode() != "icmp.slt" ||
        condition->second->operands().size() != 2) {
      continue;
    }
    const auto phiDefinition =
        definitions.find(condition->second->operands()[0]);
    if (phiDefinition == definitions.end() ||
        phiDefinition->second->opcode() != "phi" ||
        phiDefinition->second->operands().size() != 5 ||
        phiDefinition->second->operands()[0] != "int") {
      continue;
    }
    std::size_t phiCount = 0;
    for (const auto& instruction : header.instructions()) {
      if (instruction.opcode() == "phi") {
        ++phiCount;
      }
    }
    if (phiCount != 1) {
      continue;
    }

    const auto bodyName = terminator->operands()[1];
    const auto exitName = terminator->operands()[2];
    const auto* body = findBlock(function, cfg, bodyName);
    const auto* exit = findBlock(function, cfg, exitName);
    const auto headerIndex = cfg.index.find(header.name());
    const auto bodyIndex = cfg.index.find(bodyName);
    if (body == nullptr || exit == nullptr || hasLeadingPhi(*exit) ||
        headerIndex == cfg.index.end() ||
        bodyIndex == cfg.index.end() ||
        cfg.predecessors[headerIndex->second].size() != 2 ||
        cfg.predecessors[bodyIndex->second] !=
            std::vector<std::string>{header.name()} ||
        body->terminator() == nullptr ||
        body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        !hasProfitableStraightLineBody(*body)) {
      continue;
    }

    const auto& phiOperands = phiDefinition->second->operands();
    std::string nextInduction;
    if (phiOperands[2] == bodyName) {
      nextInduction = phiOperands[1];
    } else if (phiOperands[4] == bodyName) {
      nextInduction = phiOperands[3];
    } else {
      continue;
    }

    PhiCandidate candidate{header.name(),
                           bodyName,
                           exitName,
                           phiDefinition->second->result(),
                           nextInduction,
                           condition->second->operands()[1]};
    if (!hasUnitPhiUpdate(*body, candidate, definitions) ||
        !bodyResultsDoNotEscapeExcept(function, *body, candidate) ||
        !inductionPhiDoesNotEscapeLoop(function, candidate) ||
        !phiBoundIsStable(function, candidate, definitions)) {
      continue;
    }

    bool headerIsCanonical = true;
    for (std::size_t index = 0; index + 1 < header.instructions().size();
         ++index) {
      const auto& instruction = header.instructions()[index];
      if (&instruction == phiDefinition->second ||
          &instruction == condition->second) {
        continue;
      }
      headerIsCanonical = false;
      break;
    }
    if (headerIsCanonical) {
      candidates.push_back(std::move(candidate));
    }
  }
  return candidates;
}

std::unordered_set<std::string> collectValueNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  return names;
}

std::unordered_set<std::string> collectBlockNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
  }
  return names;
}

std::string freshName(std::unordered_set<std::string>& names,
                      const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::vector<Instruction> cloneInstructions(
    const std::vector<Instruction>& instructions,
    std::unordered_set<std::string>& valueNames,
    const std::string& valueStem, bool includeTerminator,
    const std::unordered_map<std::string, std::string>& blockMap = {}) {
  std::unordered_map<std::string, std::string> values;
  std::vector<Instruction> cloned;
  for (const auto& instruction : instructions) {
    if (instruction.isTerminator() && !includeTerminator) {
      continue;
    }
    auto operands = instruction.operands();
    for (auto& operand : operands) {
      const auto replacement = values.find(operand);
      if (replacement != values.end()) {
        operand = replacement->second;
      }
    }
    if (instruction.opcode() == "br") {
      const std::size_t firstTarget = operands.size() == 1 ? 0 : 1;
      for (std::size_t index = firstTarget; index < operands.size(); ++index) {
        const auto replacement = blockMap.find(operands[index]);
        if (replacement != blockMap.end()) {
          operands[index] = replacement->second;
        }
      }
    }
    std::string result;
    if (instruction.hasResult()) {
      result = freshName(valueNames, valueStem);
      values.emplace(instruction.result(), result);
    }
    cloned.emplace_back(instruction.opcode(), std::move(operands),
                        std::move(result));
  }
  return cloned;
}

struct ClonedLane {
  std::vector<Instruction> instructions;
  std::string nextInduction;
};

ClonedLane clonePhiBodyLane(
    const std::vector<Instruction>& instructions,
    const PhiCandidate& candidate, const std::string& induction,
    std::unordered_set<std::string>& valueNames,
    const std::string& valueStem) {
  std::unordered_map<std::string, std::string> values{
      {candidate.inductionPhi, induction}};
  ClonedLane cloned;
  for (const auto& instruction : instructions) {
    if (instruction.isTerminator()) {
      continue;
    }
    auto operands = instruction.operands();
    for (auto& operand : operands) {
      const auto replacement = values.find(operand);
      if (replacement != values.end()) {
        operand = replacement->second;
      }
    }
    std::string result;
    if (instruction.hasResult()) {
      result = freshName(valueNames, valueStem);
      values.emplace(instruction.result(), result);
    }
    cloned.instructions.emplace_back(instruction.opcode(),
                                     std::move(operands), result);
  }
  const auto next = values.find(candidate.nextInduction);
  if (next != values.end()) {
    cloned.nextInduction = next->second;
  }
  return cloned;
}

bool transformSlot(Function& function, const Candidate& candidate, int factor) {
  auto* header = findBlock(function, candidate.header);
  auto* body = findBlock(function, candidate.body);
  if (header == nullptr || body == nullptr) {
    return false;
  }

  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto guardName = freshName(blockNames, "sequential.unroll.guard");
  const auto tailHeaderName =
      freshName(blockNames, "sequential.unroll.tail.header");
  const auto tailBodyName =
      freshName(blockNames, "sequential.unroll.tail.body");

  const auto originalHeader = header->instructions();
  const auto originalBody = body->instructions();
  const std::unordered_map<std::string, std::string> tailBlockMap{
      {candidate.body, tailBodyName},
      {candidate.header, tailHeaderName}};

  BasicBlock tailHeader{tailHeaderName};
  tailHeader.instructions() =
      cloneInstructions(originalHeader, valueNames,
                        "%sequential.unroll.tail.header", true, tailBlockMap);

  BasicBlock tailBody{tailBodyName};
  tailBody.instructions() =
      cloneInstructions(originalBody, valueNames,
                        "%sequential.unroll.tail.body", true, tailBlockMap);

  std::vector<Instruction> unrolledBody;
  for (int lane = 0; lane < factor; ++lane) {
    auto laneInstructions =
        cloneInstructions(originalBody, valueNames,
                          "%sequential.unroll.lane" + std::to_string(lane),
                          false);
    unrolledBody.insert(unrolledBody.end(),
                        std::make_move_iterator(laneInstructions.begin()),
                        std::make_move_iterator(laneInstructions.end()));
  }
  unrolledBody.emplace_back("br",
                            std::vector<std::string>{candidate.header});
  body->instructions() = std::move(unrolledBody);

  auto headerBranch = header->instructions().back().operands();
  headerBranch[1] = guardName;
  header->instructions().back() = Instruction{"br", std::move(headerBranch)};

  const auto induction64 =
      freshName(valueNames, "%sequential.unroll.induction64");
  const auto bound64 = freshName(valueNames, "%sequential.unroll.bound64");
  const auto remaining =
      freshName(valueNames, "%sequential.unroll.remaining");
  const auto enough = freshName(valueNames, "%sequential.unroll.enough");
  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"sext", {candidate.inductionValue, "long"}, induction64});
  guard.addInstruction(
      Instruction{"sext", {candidate.bound, "long"}, bound64});
  guard.addInstruction(Instruction{"sub", {bound64, induction64}, remaining});
  guard.addInstruction(Instruction{
      "icmp.sgt", {remaining, std::to_string(factor - 1)}, enough});
  guard.addInstruction(
      Instruction{"br", {enough, candidate.body, tailHeaderName}});

  function.addBlock(std::move(guard));
  function.addBlock(std::move(tailHeader));
  function.addBlock(std::move(tailBody));
  return true;
}

bool transformPhi(Function& function, const PhiCandidate& candidate,
                  int factor) {
  auto* header = findBlock(function, candidate.header);
  auto* body = findBlock(function, candidate.body);
  if (header == nullptr || body == nullptr) {
    return false;
  }

  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto guardName = freshName(blockNames, "sequential.unroll.guard");
  const auto tailHeaderName =
      freshName(blockNames, "sequential.unroll.tail.header");
  const auto tailBodyName =
      freshName(blockNames, "sequential.unroll.tail.body");

  const auto originalBody = body->instructions();
  std::vector<Instruction> unrolledBody;
  std::string laneInduction = candidate.inductionPhi;
  for (int lane = 0; lane < factor; ++lane) {
    auto cloned =
        clonePhiBodyLane(originalBody, candidate, laneInduction, valueNames,
                         "%sequential.unroll.lane" + std::to_string(lane));
    if (cloned.nextInduction.empty()) {
      return false;
    }
    laneInduction = cloned.nextInduction;
    unrolledBody.insert(
        unrolledBody.end(),
        std::make_move_iterator(cloned.instructions.begin()),
        std::make_move_iterator(cloned.instructions.end()));
  }
  unrolledBody.emplace_back("br",
                            std::vector<std::string>{candidate.header});

  const auto tailPhi =
      freshName(valueNames, "%sequential.unroll.tail.induction");
  auto tailLane =
      clonePhiBodyLane(originalBody, candidate, tailPhi, valueNames,
                       "%sequential.unroll.tail.body");
  if (tailLane.nextInduction.empty()) {
    return false;
  }
  const auto tailCondition =
      freshName(valueNames, "%sequential.unroll.tail.condition");

  BasicBlock tailHeader{tailHeaderName};
  tailHeader.addInstruction(
      Instruction{"phi",
                  {"int", candidate.inductionPhi, guardName,
                   tailLane.nextInduction, tailBodyName},
                  tailPhi});
  tailHeader.addInstruction(
      Instruction{"icmp.slt", {tailPhi, candidate.bound}, tailCondition});
  tailHeader.addInstruction(
      Instruction{"br", {tailCondition, tailBodyName, candidate.exit}});

  BasicBlock tailBody{tailBodyName};
  tailBody.instructions() = std::move(tailLane.instructions);
  tailBody.addInstruction(Instruction{"br", {tailHeaderName}});

  for (auto& instruction : header->instructions()) {
    if (instruction.result() != candidate.inductionPhi ||
        instruction.opcode() != "phi") {
      continue;
    }
    auto operands = instruction.operands();
    for (std::size_t index = 2; index < operands.size(); index += 2) {
      if (operands[index] == candidate.body) {
        operands[index - 1] = laneInduction;
      }
    }
    instruction =
        Instruction{"phi", std::move(operands), candidate.inductionPhi};
    break;
  }
  body->instructions() = std::move(unrolledBody);

  auto headerBranch = header->instructions().back().operands();
  headerBranch[1] = guardName;
  header->instructions().back() = Instruction{"br", std::move(headerBranch)};

  const auto induction64 =
      freshName(valueNames, "%sequential.unroll.induction64");
  const auto bound64 = freshName(valueNames, "%sequential.unroll.bound64");
  const auto remaining =
      freshName(valueNames, "%sequential.unroll.remaining");
  const auto enough = freshName(valueNames, "%sequential.unroll.enough");
  BasicBlock guard{guardName};
  guard.addInstruction(
      Instruction{"sext", {candidate.inductionPhi, "long"}, induction64});
  guard.addInstruction(
      Instruction{"sext", {candidate.bound, "long"}, bound64});
  guard.addInstruction(Instruction{"sub", {bound64, induction64}, remaining});
  guard.addInstruction(Instruction{
      "icmp.sgt", {remaining, std::to_string(factor - 1)}, enough});
  guard.addInstruction(
      Instruction{"br", {enough, candidate.body, tailHeaderName}});

  function.addBlock(std::move(guard));
  function.addBlock(std::move(tailHeader));
  function.addBlock(std::move(tailBody));
  return true;
}

}  // namespace

void SequentialLoopUnrollPass::run(compiler::ir::core::Module& module) {
  if (factor_ < 2) {
    return;
  }
  for (auto& function : module.functions()) {
    const auto slotCandidates = findSlotCandidates(function);
    const auto phiCandidates = findPhiCandidates(function);
    for (const auto& candidate : slotCandidates) {
      transformSlot(function, candidate, factor_);
    }
    for (const auto& candidate : phiCandidates) {
      transformPhi(function, candidate, factor_);
    }
  }
}

}  // namespace compiler::midend::pass
