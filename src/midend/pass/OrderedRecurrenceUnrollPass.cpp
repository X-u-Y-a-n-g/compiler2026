#include "../../../include/compiler/midend/pass/OrderedRecurrenceUnrollPass.h"

#include <algorithm>
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

constexpr std::size_t kUnrollFactor = 4;
constexpr std::size_t kMaximumBodyInstructions = 32;

struct Definition {
  const Instruction* instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct Incoming {
  std::string value;
  std::string block;
};

struct Candidate {
  std::string header;
  std::string body;
  std::string latch;
  std::string exit;
  std::string preheader;
  std::string induction;
  std::string recurrence;
  std::string initialInduction;
  std::string initialRecurrence;
  std::string nextInduction;
  std::string nextRecurrence;
  std::string bound;
  std::string type;
};

DefinitionMap buildDefinitions(const Function& function) {
  DefinitionMap definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
      }
    }
  }
  return definitions;
}

const BasicBlock* findBlock(const Function& function,
                            const std::string& name) {
  for (const auto& block : function.blocks()) {
    if (block.name() == name) return &block;
  }
  return nullptr;
}

BasicBlock* findBlock(Function& function, const std::string& name) {
  for (auto& block : function.blocks()) {
    if (block.name() == name) return &block;
  }
  return nullptr;
}

std::vector<std::string> predecessors(const Function& function,
                                      const std::string& target) {
  std::vector<std::string> result;
  for (const auto& block : function.blocks()) {
    const auto* terminator = block.terminator();
    if (terminator == nullptr || terminator->opcode() != "br") continue;
    const auto& operands = terminator->operands();
    const std::size_t first = operands.size() == 1 ? 0 : 1;
    for (std::size_t index = first; index < operands.size(); ++index) {
      if (operands[index] == target &&
          std::find(result.begin(), result.end(), block.name()) ==
              result.end()) {
        result.push_back(block.name());
      }
    }
  }
  return result;
}

std::optional<std::vector<Incoming>> phiIncoming(
    const Instruction& phi) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5) {
    return std::nullopt;
  }
  return std::vector<Incoming>{
      Incoming{phi.operands()[1], phi.operands()[2]},
      Incoming{phi.operands()[3], phi.operands()[4]}};
}

bool isAddOne(const Instruction& instruction, const std::string& value) {
  if (instruction.opcode() != "add" || instruction.operands().size() != 2 ||
      !instruction.hasResult()) {
    return false;
  }
  return (instruction.operands()[0] == value &&
          detail::isOne(instruction.operands()[1])) ||
         (instruction.operands()[1] == value &&
          detail::isOne(instruction.operands()[0]));
}

bool transitivelyDependsOn(const std::string& value,
                           const std::string& dependency,
                           const DefinitionMap& definitions,
                           std::unordered_set<std::string>& visited) {
  if (value == dependency) return true;
  if (value.empty() || value.front() != '%' || !visited.insert(value).second) {
    return false;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end()) return false;
  for (const auto& operand : found->second.instruction->operands()) {
    if (transitivelyDependsOn(operand, dependency, definitions, visited)) {
      return true;
    }
  }
  return false;
}

bool dependsOn(const std::string& value, const std::string& dependency,
               const DefinitionMap& definitions) {
  std::unordered_set<std::string> visited;
  return transitivelyDependsOn(value, dependency, definitions, visited);
}

std::optional<Candidate> findCandidate(const Function& function) {
  const bool trace =
      std::getenv("COMPILER_ORDERED_RECURRENCE_UNROLL_STATS") != nullptr;
  const auto report = [&](const BasicBlock& header, const char* reason) {
    if (trace) {
      std::cerr << "ordered-recurrence-unroll: " << function.name() << ':'
                << header.name() << ": " << reason << '\n';
    }
  };
  const auto definitions = buildDefinitions(function);
  for (const auto& header : function.blocks()) {
    const auto* terminator = header.terminator();
    if (terminator == nullptr || terminator->opcode() != "br" ||
        terminator->operands().size() != 3) {
      continue;
    }
    const auto compare = definitions.find(terminator->operands()[0]);
    if (trace) {
      std::size_t phiCount = 0;
      for (const auto& instruction : header.instructions()) {
        phiCount += instruction.opcode() == "phi" ? 1U : 0U;
      }
      std::cerr << "ordered-recurrence-unroll: inspect "
                << function.name() << ':' << header.name()
                << " phis=" << phiCount << " condition="
                << (compare == definitions.end()
                        ? "<missing>"
                        : compare->second.instruction->opcode())
                << '\n';
    }
    if (compare == definitions.end() || compare->second.block != header.name() ||
        compare->second.instruction->opcode() != "icmp.slt" ||
        compare->second.instruction->operands().size() != 2) {
      continue;
    }
    const std::string bodyName = terminator->operands()[1];
    const std::string exitName = terminator->operands()[2];
    const auto* body = findBlock(function, bodyName);
    if (body == nullptr || body->terminator() == nullptr ||
        body->terminator()->opcode() != "br" ||
        body->terminator()->operands().size() != 1 ||
        body->instructions().size() < 2 ||
        body->instructions().size() - 1 > kMaximumBodyInstructions) {
      if (trace && body != nullptr) {
        std::cerr << "  body=" << bodyName
                  << " instructions=" << body->instructions().size()
                  << " terminator="
                  << (body->terminator() == nullptr
                          ? "<missing>"
                          : body->terminator()->opcode())
                  << " targets=";
        if (body->terminator() != nullptr) {
          for (const auto& operand : body->terminator()->operands()) {
            std::cerr << operand << ',';
          }
        }
        std::cerr << '\n';
      }
      report(header, "single-body-shape");
      continue;
    }
    std::string latchName = bodyName;
    if (body->terminator()->operands()[0] != header.name()) {
      latchName = body->terminator()->operands()[0];
      const auto* latch = findBlock(function, latchName);
      if (latch == nullptr || latch->instructions().size() != 1 ||
          latch->terminator() == nullptr ||
          latch->terminator()->opcode() != "br" ||
          latch->terminator()->operands() !=
              std::vector<std::string>{header.name()}) {
        report(header, "empty-latch");
        continue;
      }
    }

    std::vector<const Instruction*> phis;
    for (const auto& instruction : header.instructions()) {
      if (instruction.opcode() == "phi") phis.push_back(&instruction);
    }
    if (phis.size() != 2) continue;

    const std::string induction =
        compare->second.instruction->operands()[0];
    const std::string bound = compare->second.instruction->operands()[1];
    const Instruction* inductionPhi = nullptr;
    const Instruction* recurrencePhi = nullptr;
    for (const auto* phi : phis) {
      if (phi->result() == induction) {
        inductionPhi = phi;
      } else {
        recurrencePhi = phi;
      }
    }
    if (inductionPhi == nullptr || recurrencePhi == nullptr ||
        inductionPhi->operands().empty() ||
        recurrencePhi->operands().empty() ||
        inductionPhi->operands()[0] != "int" ||
        recurrencePhi->operands()[0] != "int") {
      report(header, "phi-types");
      continue;
    }

    const auto inductionIncoming = phiIncoming(*inductionPhi);
    const auto recurrenceIncoming = phiIncoming(*recurrencePhi);
    if (!inductionIncoming.has_value() || !recurrenceIncoming.has_value()) {
      report(header, "phi-incoming");
      continue;
    }
    std::string preheader;
    std::string initialInduction;
    std::string nextInduction;
    for (const auto& incoming : *inductionIncoming) {
      if (incoming.block == latchName) {
        nextInduction = incoming.value;
      } else {
        preheader = incoming.block;
        initialInduction = incoming.value;
      }
    }
    std::string recurrencePreheader;
    std::string initialRecurrence;
    std::string nextRecurrence;
    for (const auto& incoming : *recurrenceIncoming) {
      if (incoming.block == latchName) {
        nextRecurrence = incoming.value;
      } else {
        recurrencePreheader = incoming.block;
        initialRecurrence = incoming.value;
      }
    }
    if (preheader.empty() || preheader != recurrencePreheader ||
        nextInduction.empty() || nextRecurrence.empty()) {
      report(header, "preheader-pairs");
      continue;
    }
    const auto* preheaderBlock = findBlock(function, preheader);
    if (preheaderBlock == nullptr || preheaderBlock->terminator() == nullptr ||
        preheaderBlock->terminator()->opcode() != "br" ||
        preheaderBlock->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      report(header, "preheader-edge");
      continue;
    }
    auto headerPredecessors = predecessors(function, header.name());
    std::sort(headerPredecessors.begin(), headerPredecessors.end());
    auto expectedPredecessors = std::vector<std::string>{latchName, preheader};
    std::sort(expectedPredecessors.begin(), expectedPredecessors.end());
    if (headerPredecessors != expectedPredecessors) {
      report(header, "header-predecessors");
      continue;
    }

    const auto nextInductionDefinition = definitions.find(nextInduction);
    const auto nextRecurrenceDefinition = definitions.find(nextRecurrence);
    if (nextInductionDefinition == definitions.end() ||
        (nextInductionDefinition->second.block != bodyName &&
         nextInductionDefinition->second.block != latchName) ||
        !isAddOne(*nextInductionDefinition->second.instruction, induction) ||
        nextRecurrenceDefinition == definitions.end() ||
        (nextRecurrenceDefinition->second.block != bodyName &&
         nextRecurrenceDefinition->second.block != latchName) ||
        nextRecurrenceDefinition->second.instruction->opcode() != "select" ||
        nextRecurrenceDefinition->second.instruction->operands().size() != 3) {
      report(header, "recurrence-shape");
      continue;
    }
    const auto& selectOperands =
        nextRecurrenceDefinition->second.instruction->operands();
    if (selectOperands[1] != recurrencePhi->result() &&
        selectOperands[2] != recurrencePhi->result()) {
      report(header, "select-carry");
      continue;
    }
    if (!dependsOn(selectOperands[0], recurrencePhi->result(), definitions)) {
      report(header, "select-condition");
      continue;
    }

    std::size_t loads = 0;
    bool rejected = false;
    for (const auto& instruction : body->instructions()) {
      if (instruction.isTerminator()) continue;
      if (instruction.opcode() == "load") {
        ++loads;
      } else if (instruction.opcode() == "store" ||
                 instruction.opcode() == "call" ||
                 instruction.opcode() == "alloca" ||
                 instruction.opcode() == "phi") {
        rejected = true;
        break;
      }
    }
    if (rejected || loads == 0 || loads > 4) {
      report(header, "body-effects");
      continue;
    }

    return Candidate{header.name(),
                     bodyName,
                     latchName,
                     exitName,
                     preheader,
                     inductionPhi->result(),
                     recurrencePhi->result(),
                     initialInduction,
                     initialRecurrence,
                     nextInduction,
                     nextRecurrence,
                     bound,
                     recurrencePhi->operands()[0]};
  }
  return std::nullopt;
}

std::string freshName(std::unordered_set<std::string>& names,
                      const std::string& stem, bool value) {
  std::string result = value ? "%" + stem : stem;
  std::size_t suffix = 0;
  while (!names.insert(result).second) {
    result = (value ? "%" : "") + stem + "." + std::to_string(++suffix);
  }
  return result;
}

bool transform(Function& function, const Candidate& candidate) {
  auto* preheader = findBlock(function, candidate.preheader);
  auto* header = findBlock(function, candidate.header);
  const auto* body = findBlock(function, candidate.body);
  if (preheader == nullptr || header == nullptr || body == nullptr ||
      preheader->terminator() == nullptr) {
    return false;
  }

  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  const auto mainHeaderName =
      freshName(names, "ordered.unroll.header", false);
  const auto mainBodyName = freshName(names, "ordered.unroll.body", false);
  const auto mainInduction =
      freshName(names, "ordered.unroll.index", true);
  const auto mainRecurrence =
      freshName(names, "ordered.unroll.current", true);
  const auto boundLargeEnough =
      freshName(names, "ordered.unroll.bound.ok", true);
  const auto lastStart = freshName(names, "ordered.unroll.last.start", true);
  const auto enoughIndices =
      freshName(names, "ordered.unroll.index.ok", true);
  const auto enough = freshName(names, "ordered.unroll.enough", true);

  BasicBlock mainBody{mainBodyName};
  std::string currentInduction = mainInduction;
  std::string currentRecurrence = mainRecurrence;
  for (std::size_t lane = 0; lane < kUnrollFactor; ++lane) {
    std::unordered_map<std::string, std::string> aliases{
        {candidate.induction, currentInduction},
        {candidate.recurrence, currentRecurrence}};
    for (const auto& instruction : body->instructions()) {
      if (instruction.isTerminator()) continue;
      std::string result;
      if (instruction.hasResult()) {
        result = freshName(
            names, "ordered.unroll.lane" + std::to_string(lane), true);
        aliases[instruction.result()] = result;
      }
      mainBody.addInstruction(
          Instruction{instruction.opcode(),
                      detail::rewriteOperands(instruction.operands(), aliases),
                      result});
    }
    if (candidate.latch != candidate.body) {
      const auto* latch = findBlock(function, candidate.latch);
      if (latch == nullptr) return false;
      for (const auto& instruction : latch->instructions()) {
        if (instruction.isTerminator()) continue;
        std::string result;
        if (instruction.hasResult()) {
          result = freshName(
              names, "ordered.unroll.lane" + std::to_string(lane), true);
          aliases[instruction.result()] = result;
        }
        mainBody.addInstruction(
            Instruction{instruction.opcode(),
                        detail::rewriteOperands(instruction.operands(), aliases),
                        result});
      }
    }
    currentInduction = aliases.at(candidate.nextInduction);
    currentRecurrence = aliases.at(candidate.nextRecurrence);
  }
  mainBody.addInstruction(Instruction{"br", {mainHeaderName}});

  BasicBlock mainHeader{mainHeaderName};
  mainHeader.addInstruction(
      Instruction{"phi",
                  {"int", candidate.initialInduction, candidate.preheader,
                   currentInduction, mainBodyName},
                  mainInduction});
  mainHeader.addInstruction(
      Instruction{"phi",
                  {candidate.type, candidate.initialRecurrence,
                   candidate.preheader, currentRecurrence, mainBodyName},
                  mainRecurrence});
  mainHeader.addInstruction(
      Instruction{"icmp.sge", {candidate.bound, "4"}, boundLargeEnough});
  mainHeader.addInstruction(
      Instruction{"sub", {candidate.bound, "3"}, lastStart});
  mainHeader.addInstruction(
      Instruction{"icmp.slt", {mainInduction, lastStart}, enoughIndices});
  mainHeader.addInstruction(
      Instruction{"and", {boundLargeEnough, enoughIndices}, enough});
  mainHeader.addInstruction(
      Instruction{"br", {enough, mainBodyName, candidate.header}});

  preheader->instructions().back() = Instruction{"br", {mainHeaderName}};
  for (auto& instruction : header->instructions()) {
    if (instruction.opcode() != "phi") continue;
    if (instruction.result() == candidate.induction) {
      instruction =
          Instruction{"phi",
                      {"int", mainInduction, mainHeaderName,
                       candidate.nextInduction, candidate.latch},
                      candidate.induction};
    } else if (instruction.result() == candidate.recurrence) {
      instruction =
          Instruction{"phi",
                      {candidate.type, mainRecurrence, mainHeaderName,
                       candidate.nextRecurrence, candidate.latch},
                      candidate.recurrence};
    }
  }

  function.addBlock(std::move(mainHeader));
  function.addBlock(std::move(mainBody));
  return true;
}

}  // namespace

void OrderedRecurrenceUnrollPass::run(
    compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_ORDERED_RECURRENCE_UNROLL") != nullptr) {
    return;
  }
  for (auto& function : module.functions()) {
    if (const auto candidate = findCandidate(function)) {
      transform(function, *candidate);
    }
  }
}

}  // namespace compiler::midend::pass
