#include "../../../include/compiler/midend/pass/AffineModularRecurrencePass.h"

#include <algorithm>
#include <limits>
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

struct CFG {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> predecessors;
};

struct PhiIncoming {
  std::string value;
  std::string block;
};

struct Candidate {
  std::string preheader;
  std::string header;
  std::string body;
  std::string exit;
  std::string type;
  std::string statePhi;
  std::string counterPhi;
  std::string initialState;
  std::string count;
  std::string step;
  std::string modulus;
  bool hasModulus = false;
  bool stepIsLiteral = false;
  bool modulusIsLiteral = false;
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
    const auto addTarget = [&](const std::string& target) {
      const auto found = cfg.index.find(target);
      if (found != cfg.index.end()) {
        cfg.predecessors[found->second].push_back(block.name());
      }
    };
    const auto& operands = terminator->operands();
    if (operands.size() == 1) {
      addTarget(operands[0]);
    } else if (operands.size() == 3) {
      addTarget(operands[1]);
      if (operands[2] != operands[1]) {
        addTarget(operands[2]);
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
  return found == cfg.index.end() ? nullptr : &function.blocks()[found->second];
}

BasicBlock* findBlock(Function& function, const std::string& name) {
  for (auto& block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

std::optional<std::vector<PhiIncoming>> parsePhi(
    const Instruction& instruction) {
  if (instruction.opcode() != "phi" || !instruction.hasResult() ||
      instruction.operands().size() < 5 ||
      instruction.operands().size() % 2 == 0) {
    return std::nullopt;
  }
  std::vector<PhiIncoming> incoming;
  for (std::size_t index = 1; index + 1 < instruction.operands().size();
       index += 2) {
    incoming.push_back(PhiIncoming{instruction.operands()[index],
                                   instruction.operands()[index + 1]});
  }
  return incoming;
}

std::optional<std::string> incomingValue(
    const std::vector<PhiIncoming>& incoming, const std::string& block) {
  std::optional<std::string> result;
  for (const auto& edge : incoming) {
    if (edge.block != block) {
      continue;
    }
    if (result.has_value()) {
      return std::nullopt;
    }
    result = edge.value;
  }
  return result;
}

bool isDefinedInBlocks(const Function& function, const std::string& value,
                       const std::unordered_set<std::string>& blockNames) {
  for (const auto& block : function.blocks()) {
    if (blockNames.find(block.name()) == blockNames.end()) {
      continue;
    }
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && instruction.result() == value) {
        return true;
      }
    }
  }
  return false;
}

std::optional<bool> classifyLoopInvariantI32Operand(
    const Function& function, const std::string& operand,
    const std::unordered_set<std::string>& loopBlocks,
    bool requirePositiveLiteral) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(operand, literal)) {
    if (literal < std::numeric_limits<int>::min() ||
        literal > std::numeric_limits<int>::max() ||
        (requirePositiveLiteral && literal <= 0)) {
      return std::nullopt;
    }
    return true;
  }
  if (operand.empty() || operand.front() != '%' ||
      isDefinedInBlocks(function, operand, loopBlocks)) {
    return std::nullopt;
  }
  return false;
}

bool isUnitIncrement(const Instruction& instruction, const std::string& value) {
  if (instruction.opcode() != "add" || !instruction.hasResult() ||
      instruction.operands().size() != 2) {
    return false;
  }
  long long literal = 0;
  return (instruction.operands()[0] == value &&
          detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
          literal == 1) ||
         (instruction.operands()[1] == value &&
          detail::parseIntegerLiteral(instruction.operands()[0], literal) &&
          literal == 1);
}

std::optional<std::pair<std::string, std::string>> affineAdd(
    const Instruction& instruction, const std::string& value) {
  if (instruction.opcode() != "add" || !instruction.hasResult() ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }
  if (instruction.operands()[0] == value &&
      instruction.operands()[1] != value) {
    return std::pair{instruction.result(), instruction.operands()[1]};
  }
  if (instruction.operands()[1] == value &&
      instruction.operands()[0] != value) {
    return std::pair{instruction.result(), instruction.operands()[0]};
  }
  return std::nullopt;
}

bool onlyExpectedInstructions(
    const BasicBlock& block,
    const std::unordered_set<std::string>& expectedResults) {
  for (const auto& instruction : block.instructions()) {
    if (instruction.isTerminator()) {
      continue;
    }
    if (!instruction.hasResult() ||
        expectedResults.find(instruction.result()) == expectedResults.end()) {
      return false;
    }
  }
  return true;
}

bool usesOnlyInBlocks(const Function& function, const std::string& value,
                      const std::unordered_set<std::string>& allowedBlocks) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (operand != value) {
          continue;
        }
        if (allowedBlocks.find(block.name()) == allowedBlocks.end()) {
          return false;
        }
      }
    }
  }
  return true;
}

std::optional<Candidate> findCandidate(
    const Function& function,
    const std::unordered_set<std::string>& transformedHeaders) {
  const auto cfg = buildCFG(function);
  const auto definitions = buildDefinitions(function);

  for (const auto& header : function.blocks()) {
    if (transformedHeaders.find(header.name()) != transformedHeaders.end()) {
      continue;
    }
    const auto* headerTerminator = header.terminator();
    if (headerTerminator == nullptr || headerTerminator->opcode() != "br" ||
        headerTerminator->operands().size() != 3) {
      continue;
    }
    const auto condition = definitions.find(headerTerminator->operands()[0]);
    if (condition == definitions.end() ||
        condition->second->opcode() != "icmp.slt" ||
        condition->second->operands().size() != 2) {
      continue;
    }

    const std::string counterPhi = condition->second->operands()[0];
    const std::string count = condition->second->operands()[1];
    const auto counterDefinition = definitions.find(counterPhi);
    if (counterDefinition == definitions.end() ||
        counterDefinition->second->opcode() != "phi" ||
        counterDefinition->second->operands().empty() ||
        counterDefinition->second->operands()[0] != "int") {
      continue;
    }
    const std::string bodyName = headerTerminator->operands()[1];
    const std::string exitName = headerTerminator->operands()[2];
    const std::unordered_set<std::string> loopBlocks{header.name(), bodyName};
    const auto* body = findBlock(function, cfg, bodyName);
    const auto* exit = findBlock(function, cfg, exitName);
    if (body == nullptr || exit == nullptr || body->terminator() == nullptr ||
        body->terminator()->opcode() != "br" ||
        body->terminator()->operands() !=
            std::vector<std::string>{header.name()} ||
        (!exit->instructions().empty() &&
         exit->instructions().front().opcode() == "phi")) {
      continue;
    }

    const auto headerIndex = cfg.index.at(header.name());
    const auto bodyIndex = cfg.index.at(bodyName);
    const auto exitIndex = cfg.index.at(exitName);
    if (cfg.predecessors[headerIndex].size() != 2 ||
        cfg.predecessors[bodyIndex] !=
            std::vector<std::string>{header.name()} ||
        cfg.predecessors[exitIndex] !=
            std::vector<std::string>{header.name()}) {
      continue;
    }
    std::string preheaderName;
    for (const auto& predecessor : cfg.predecessors[headerIndex]) {
      if (predecessor != bodyName) {
        preheaderName = predecessor;
      }
    }
    const auto* preheader = findBlock(function, cfg, preheaderName);
    if (preheader == nullptr || preheader->terminator() == nullptr ||
        preheader->terminator()->opcode() != "br" ||
        preheader->terminator()->operands() !=
            std::vector<std::string>{header.name()}) {
      continue;
    }

    const auto counterIncoming = parsePhi(*counterDefinition->second);
    if (!counterIncoming.has_value()) {
      continue;
    }
    const auto counterInitial = incomingValue(*counterIncoming, preheaderName);
    const auto counterNext = incomingValue(*counterIncoming, bodyName);
    long long initialCounter = 0;
    if (!counterInitial.has_value() || !counterNext.has_value() ||
        !detail::parseIntegerLiteral(*counterInitial, initialCounter) ||
        initialCounter != 0) {
      continue;
    }
    const auto counterNextDefinition = definitions.find(*counterNext);
    if (counterNextDefinition == definitions.end() ||
        !isUnitIncrement(*counterNextDefinition->second, counterPhi)) {
      continue;
    }

    const Instruction* statePhiInstruction = nullptr;
    std::size_t phiCount = 0;
    for (const auto& instruction : header.instructions()) {
      if (instruction.opcode() != "phi") {
        continue;
      }
      ++phiCount;
      if (instruction.result() != counterPhi) {
        statePhiInstruction = &instruction;
      }
    }
    if (phiCount != 2 || statePhiInstruction == nullptr ||
        header.instructions().size() != 4 ||
        statePhiInstruction->operands().empty() ||
        statePhiInstruction->operands()[0] != "int" || count == counterPhi ||
        count == statePhiInstruction->result() ||
        isDefinedInBlocks(function, count, loopBlocks)) {
      continue;
    }
    const std::unordered_set<std::string> expectedHeaderResults{
        counterPhi, statePhiInstruction->result(), condition->second->result()};
    if (!onlyExpectedInstructions(header, expectedHeaderResults)) {
      continue;
    }
    const auto stateIncoming = parsePhi(*statePhiInstruction);
    if (!stateIncoming.has_value()) {
      continue;
    }
    const auto initialState = incomingValue(*stateIncoming, preheaderName);
    const auto nextState = incomingValue(*stateIncoming, bodyName);
    if (!initialState.has_value() || !nextState.has_value()) {
      continue;
    }
    if (isDefinedInBlocks(function, *initialState, loopBlocks)) {
      continue;
    }
    const auto nextStateDefinition = definitions.find(*nextState);
    if (nextStateDefinition == definitions.end()) {
      continue;
    }
    bool hasModulus = false;
    bool modulusIsLiteral = false;
    std::string modulus;
    const Instruction* addInstruction = nullptr;
    if (nextStateDefinition->second->opcode() == "srem" &&
        nextStateDefinition->second->operands().size() == 2) {
      hasModulus = true;
      modulus = nextStateDefinition->second->operands()[1];
      const auto modulusKind =
          classifyLoopInvariantI32Operand(function, modulus, loopBlocks, true);
      if (!modulusKind.has_value()) {
        continue;
      }
      modulusIsLiteral = *modulusKind;
      const auto addDefinition =
          definitions.find(nextStateDefinition->second->operands()[0]);
      if (addDefinition == definitions.end()) {
        continue;
      }
      addInstruction = addDefinition->second;
    } else if (nextStateDefinition->second->opcode() == "add") {
      addInstruction = nextStateDefinition->second;
    } else {
      continue;
    }
    const auto add = affineAdd(*addInstruction, statePhiInstruction->result());
    if (!add.has_value()) {
      continue;
    }
    const auto stepKind = classifyLoopInvariantI32Operand(
        function, add->second, loopBlocks, hasModulus);
    if (!stepKind.has_value()) {
      continue;
    }
    std::unordered_set<std::string> expectedBodyResults{add->first,
                                                        *counterNext};
    if (hasModulus) {
      expectedBodyResults.insert(*nextState);
    }
    if (!onlyExpectedInstructions(*body, expectedBodyResults) ||
        body->instructions().size() != expectedBodyResults.size() + 1) {
      continue;
    }

    Candidate candidate{preheaderName,
                        header.name(),
                        bodyName,
                        exitName,
                        statePhiInstruction->operands()[0],
                        statePhiInstruction->result(),
                        counterPhi,
                        *initialState,
                        count,
                        add->second,
                        modulus,
                        hasModulus,
                        *stepKind,
                        modulusIsLiteral};
    if (usesOnlyInBlocks(function, candidate.statePhi,
                         {candidate.body, candidate.exit}) &&
        usesOnlyInBlocks(function, candidate.counterPhi,
                         {candidate.header, candidate.body})) {
      return candidate;
    }
  }
  return std::nullopt;
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

void replaceOperandInBlock(BasicBlock& block, const std::string& from,
                           const std::string& to) {
  std::vector<Instruction> rewritten;
  rewritten.reserve(block.instructions().size());
  for (const auto& instruction : block.instructions()) {
    auto operands = instruction.operands();
    for (auto& operand : operands) {
      if (operand == from) {
        operand = to;
      }
    }
    rewritten.emplace_back(instruction.opcode(), std::move(operands),
                           instruction.result());
  }
  block.instructions() = std::move(rewritten);
}

bool transform(Function& function, const Candidate& candidate) {
  auto* preheader = findBlock(function, candidate.preheader);
  auto* exit = findBlock(function, candidate.exit);
  if (preheader == nullptr || exit == nullptr) {
    return false;
  }

  auto valueNames = collectValueNames(function);
  auto blockNames = collectBlockNames(function);
  const auto fastBlockName = freshName(blockNames, "affine.mod.fast");
  const auto initialNonNegative =
      freshName(valueNames, "%affine.mod.initial.nonnegative");
  const auto countPositive =
      freshName(valueNames, "%affine.mod.count.positive");
  const auto initial64 = freshName(valueNames, "%affine.mod.initial64");
  const auto count64 = freshName(valueNames, "%affine.mod.count64");
  const auto stepPositive = freshName(valueNames, "%affine.mod.step.positive");
  const auto modulusPositive =
      freshName(valueNames, "%affine.mod.modulus.positive");
  const auto step64 = freshName(valueNames, "%affine.mod.step64");
  const auto modulus64 = freshName(valueNames, "%affine.mod.modulus64");
  const auto scaled64 = freshName(valueNames, "%affine.mod.scaled64");
  const auto total64 = freshName(valueNames, "%affine.mod.total64");
  const auto fitsI32 = freshName(valueNames, "%affine.mod.fits.i32");
  const auto nonNegative = freshName(valueNames, "%affine.mod.nonnegative");
  const auto positiveStepGuard =
      freshName(valueNames, "%affine.mod.positive.step.guard");
  const auto positiveModulusGuard =
      freshName(valueNames, "%affine.mod.positive.modulus.guard");
  const auto guard = freshName(valueNames, "%affine.mod.guard");
  const auto remainder64 = freshName(valueNames, "%affine.mod.remainder64");
  const auto fastState = freshName(valueNames, "%affine.mod.state");
  const auto mergedState = freshName(valueNames, "%affine.mod.merged");

  auto& preheaderInstructions = preheader->instructions();
  auto insert = preheaderInstructions.end() - 1;
  auto append = [&](Instruction instruction) {
    insert = preheaderInstructions.insert(insert, std::move(instruction));
    ++insert;
  };
  if (candidate.hasModulus) {
    append(Instruction{
        "icmp.sge", {candidate.initialState, "0"}, initialNonNegative});
  }
  // A zero-trip modular loop returns the unmodified initial value, which is not
  // generally congruent to initial % modulus. Plain addition handles zero
  // trips.
  append(Instruction{candidate.hasModulus ? "icmp.sgt" : "icmp.sge",
                     {candidate.count, "0"},
                     countPositive});
  append(Instruction{"sext", {candidate.initialState, "long"}, initial64});
  append(Instruction{"sext", {candidate.count, "long"}, count64});
  std::string positivityGuard = countPositive;
  const bool needsEarlyPositivity =
      candidate.hasModulus &&
      (!candidate.stepIsLiteral || !candidate.modulusIsLiteral);
  if (needsEarlyPositivity) {
    append(
        Instruction{"and", {initialNonNegative, countPositive}, nonNegative});
    positivityGuard = nonNegative;
  }
  std::string step64Operand = candidate.step;
  if (!candidate.stepIsLiteral) {
    append(Instruction{"sext", {candidate.step, "long"}, step64});
    if (candidate.hasModulus) {
      append(Instruction{"icmp.sgt", {candidate.step, "0"}, stepPositive});
      append(Instruction{
          "and", {positivityGuard, stepPositive}, positiveStepGuard});
      positivityGuard = positiveStepGuard;
    }
    step64Operand = step64;
  }
  std::string modulus64Operand = candidate.modulus;
  if (candidate.hasModulus && !candidate.modulusIsLiteral) {
    append(Instruction{"icmp.sgt", {candidate.modulus, "0"}, modulusPositive});
    append(Instruction{"sext", {candidate.modulus, "long"}, modulus64});
    append(Instruction{
        "and", {positivityGuard, modulusPositive}, positiveModulusGuard});
    positivityGuard = positiveModulusGuard;
    modulus64Operand = modulus64;
  }
  append(Instruction{"mul", {count64, step64Operand}, scaled64});
  append(Instruction{"add", {initial64, scaled64}, total64});
  std::string guardOperand = positivityGuard;
  if (candidate.hasModulus) {
    append(Instruction{"icmp.sle", {total64, "2147483647"}, fitsI32});
    if (!needsEarlyPositivity) {
      append(
          Instruction{"and", {initialNonNegative, countPositive}, nonNegative});
      positivityGuard = nonNegative;
    }
    append(Instruction{"and", {positivityGuard, fitsI32}, guard});
    guardOperand = guard;
  }
  preheaderInstructions.back() =
      Instruction{"br", {guardOperand, fastBlockName, candidate.header}};

  BasicBlock fastBlock{fastBlockName};
  std::string final64 = total64;
  if (candidate.hasModulus) {
    fastBlock.addInstruction(
        Instruction{"srem", {total64, modulus64Operand}, remainder64});
    final64 = remainder64;
  }
  fastBlock.addInstruction(
      Instruction{"trunc", {final64, candidate.type}, fastState});
  fastBlock.addInstruction(Instruction{"br", {candidate.exit}});

  replaceOperandInBlock(*exit, candidate.statePhi, mergedState);
  exit->instructions().insert(
      exit->instructions().begin(),
      Instruction{"phi",
                  {candidate.type, candidate.statePhi, candidate.header,
                   fastState, fastBlockName},
                  mergedState});
  function.addBlock(std::move(fastBlock));
  return true;
}

}  // namespace

void AffineModularRecurrencePass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    std::unordered_set<std::string> transformedHeaders;
    while (const auto candidate = findCandidate(function, transformedHeaders)) {
      transformedHeaders.insert(candidate->header);
      if (!transform(function, *candidate)) {
        break;
      }
    }
  }
}

}  // namespace compiler::midend::pass
