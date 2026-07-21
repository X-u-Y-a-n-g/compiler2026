#include "../../../include/compiler/midend/pass/BitwiseIdiomRecognitionPass.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
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

struct LocatedInstruction {
  const Instruction* instruction = nullptr;
  std::string block;
};

struct CFG {
  std::unordered_map<std::string, const BasicBlock*> blocks;
  std::unordered_map<std::string, std::vector<std::string>> successors;
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
};

struct BitAccumulateIdiom {
  std::string opcode;
  std::string fallbackTarget;
  unsigned iterationCount = 0;
};

using Definitions = std::unordered_map<std::string, LocatedInstruction>;
using Environment = std::unordered_map<std::string, std::int64_t>;

std::optional<std::int64_t> parseInteger(const std::string& value) {
  if (value.empty()) return std::nullopt;
  char* end = nullptr;
  const auto parsed = std::strtoll(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') return std::nullopt;
  return parsed;
}

bool isIntegerParameter(const compiler::ir::core::FunctionParameter& parameter) {
  return parameter.type == "int" || parameter.type == "i32";
}

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") return {};
  if (terminator->operands().size() == 1) return {terminator->operands()[0]};
  if (terminator->operands().size() == 3) {
    return {terminator->operands()[1], terminator->operands()[2]};
  }
  return {};
}

std::optional<CFG> buildCFG(const Function& function) {
  CFG cfg;
  for (const auto& block : function.blocks()) {
    if (!cfg.blocks.emplace(block.name(), &block).second) return std::nullopt;
  }
  for (const auto& block : function.blocks()) {
    const auto targets = branchTargets(block);
    const auto* terminator = block.terminator();
    if (terminator != nullptr && terminator->opcode() == "br" && targets.empty()) {
      return std::nullopt;
    }
    for (const auto& target : targets) {
      if (cfg.blocks.find(target) == cfg.blocks.end()) return std::nullopt;
      cfg.successors[block.name()].push_back(target);
      cfg.predecessors[target].push_back(block.name());
    }
  }
  return cfg;
}

Definitions buildDefinitions(const Function& function) {
  Definitions definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            LocatedInstruction{&instruction, block.name()});
      }
    }
  }
  return definitions;
}

std::optional<std::string> phiIncomingFrom(const Instruction& phi,
                                           const std::string& predecessor) {
  if (phi.opcode() != "phi" || phi.operands().size() < 3 ||
      phi.operands().size() % 2 == 0) {
    return std::nullopt;
  }
  std::optional<std::string> incoming;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] != predecessor) continue;
    if (incoming.has_value()) return std::nullopt;
    incoming = phi.operands()[index];
  }
  return incoming;
}

bool phiHasExactlyPredecessors(const Instruction& phi,
                               const std::string& first,
                               const std::string& second) {
  if (phi.opcode() != "phi" || phi.operands().size() != 5) return false;
  std::unordered_set<std::string> predecessors;
  predecessors.insert(phi.operands()[2]);
  predecessors.insert(phi.operands()[4]);
  return predecessors.size() == 2 && predecessors.count(first) != 0 &&
         predecessors.count(second) != 0;
}

const Instruction* findHeaderPhiInitializedWith(const BasicBlock& header,
                                                const std::string& preheader,
                                                const std::string& initial) {
  const Instruction* result = nullptr;
  for (const auto& instruction : header.instructions()) {
    if (const auto incoming = phiIncomingFrom(instruction, preheader);
        incoming.has_value() && *incoming == initial) {
      if (result != nullptr) return nullptr;
      result = &instruction;
    }
  }
  return result;
}

bool binaryMatches(const Instruction& instruction, const std::string& opcode,
                   const std::string& lhs, const std::string& rhs,
                   bool commutative = false) {
  if (instruction.opcode() != opcode || instruction.operands().size() != 2) {
    return false;
  }
  const bool direct = instruction.operands()[0] == lhs &&
                      instruction.operands()[1] == rhs;
  const bool reversed = commutative && instruction.operands()[0] == rhs &&
                        instruction.operands()[1] == lhs;
  return direct || reversed;
}

const Instruction* findUniqueInstruction(
    const Function& function, const std::unordered_set<std::string>& blocks,
    const std::function<bool(const Instruction&)>& predicate) {
  const Instruction* result = nullptr;
  for (const auto& block : function.blocks()) {
    if (blocks.count(block.name()) == 0) continue;
    for (const auto& instruction : block.instructions()) {
      if (!predicate(instruction)) continue;
      if (result != nullptr) return nullptr;
      result = &instruction;
    }
  }
  return result;
}

std::unordered_set<std::string> reachableBlocks(const CFG& cfg,
                                                const std::string& entry) {
  std::unordered_set<std::string> reachable;
  std::deque<std::string> worklist;
  reachable.insert(entry);
  worklist.push_back(entry);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    const auto found = cfg.successors.find(block);
    if (found == cfg.successors.end()) continue;
    for (const auto& successor : found->second) {
      if (reachable.insert(successor).second) worklist.push_back(successor);
    }
  }
  return reachable;
}

std::unordered_set<std::string> naturalLoopBlocks(const CFG& cfg,
                                                  const std::string& header,
                                                  const std::string& latch) {
  std::unordered_set<std::string> loop{header, latch};
  std::deque<std::string> worklist;
  if (latch != header) worklist.push_back(latch);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    const auto found = cfg.predecessors.find(block);
    if (found == cfg.predecessors.end()) continue;
    for (const auto& predecessor : found->second) {
      if (loop.insert(predecessor).second && predecessor != header) {
        worklist.push_back(predecessor);
      }
    }
  }
  return loop;
}

bool isAllowedInstruction(const Instruction& instruction) {
  static const std::unordered_set<std::string> allowed = {
      "phi",      "br",       "ret",      "srem",    "sdiv",
      "add",      "mul",      "sub",      "and",     "or",
      "xor",      "select",   "icmp.eq",  "icmp.ne", "icmp.slt",
      "icmp.sle", "icmp.sgt", "icmp.sge"};
  return allowed.count(instruction.opcode()) != 0;
}

std::optional<std::int64_t> evaluateValue(
    const std::string& value, const std::string& predecessor,
    const Definitions& definitions, const Environment& environment,
    unsigned depth = 0) {
  if (depth > 32) return std::nullopt;
  if (const auto known = environment.find(value); known != environment.end()) {
    return known->second;
  }
  if (const auto literal = parseInteger(value); literal.has_value()) return literal;
  const auto definition = definitions.find(value);
  if (definition == definitions.end()) return std::nullopt;
  const auto& instruction = *definition->second.instruction;
  if (instruction.opcode() == "phi") {
    const auto incoming = phiIncomingFrom(instruction, predecessor);
    if (!incoming.has_value()) return std::nullopt;
    return evaluateValue(*incoming, predecessor, definitions, environment,
                         depth + 1);
  }
  if (instruction.opcode() == "select" && instruction.operands().size() == 3) {
    const auto condition = evaluateValue(instruction.operands()[0], predecessor,
                                         definitions, environment, depth + 1);
    if (!condition.has_value()) return std::nullopt;
    return evaluateValue(instruction.operands()[*condition != 0 ? 1 : 2],
                         predecessor, definitions, environment, depth + 1);
  }
  if (instruction.operands().size() != 2) return std::nullopt;
  const auto lhs = evaluateValue(instruction.operands()[0], predecessor,
                                 definitions, environment, depth + 1);
  const auto rhs = evaluateValue(instruction.operands()[1], predecessor,
                                 definitions, environment, depth + 1);
  if (!lhs.has_value() || !rhs.has_value()) return std::nullopt;
  const auto& opcode = instruction.opcode();
  if (opcode == "add") return *lhs + *rhs;
  if (opcode == "sub") return *lhs - *rhs;
  if (opcode == "mul") return *lhs * *rhs;
  if (opcode == "and") return *lhs & *rhs;
  if (opcode == "or") return *lhs | *rhs;
  if (opcode == "xor") return *lhs ^ *rhs;
  if (opcode == "icmp.eq") return *lhs == *rhs;
  if (opcode == "icmp.ne") return *lhs != *rhs;
  if (opcode == "icmp.slt") return *lhs < *rhs;
  if (opcode == "icmp.sle") return *lhs <= *rhs;
  if (opcode == "icmp.sgt") return *lhs > *rhs;
  if (opcode == "icmp.sge") return *lhs >= *rhs;
  return std::nullopt;
}

std::optional<bool> resolveAccumulatorChoice(
    const std::string& value, const std::string& block,
    const std::string& predecessor, const std::string& accumulator,
    const std::string& accumulated, const Definitions& definitions,
    const Environment& environment, unsigned depth = 0) {
  if (depth > 16) return std::nullopt;
  if (value == accumulator) return false;
  if (value == accumulated) return true;
  const auto definition = definitions.find(value);
  if (definition == definitions.end() || definition->second.block != block) {
    return std::nullopt;
  }
  const auto& instruction = *definition->second.instruction;
  if (instruction.opcode() == "phi") {
    const auto incoming = phiIncomingFrom(instruction, predecessor);
    if (!incoming.has_value()) return std::nullopt;
    return resolveAccumulatorChoice(*incoming, block, predecessor, accumulator,
                                    accumulated, definitions, environment,
                                    depth + 1);
  }
  if (instruction.opcode() == "select" && instruction.operands().size() == 3) {
    const auto condition = evaluateValue(instruction.operands()[0], predecessor,
                                         definitions, environment);
    if (!condition.has_value()) return std::nullopt;
    return resolveAccumulatorChoice(
        instruction.operands()[*condition != 0 ? 1 : 2], block, predecessor,
        accumulator, accumulated, definitions, environment, depth + 1);
  }
  return std::nullopt;
}

std::optional<bool> simulateIteration(
    const CFG& cfg, const Definitions& definitions, const std::string& header,
    const std::string& body, const std::string& accumulator,
    const std::string& accumulated, const Instruction& accumulatorPhi,
    const Environment& environment) {
  std::string predecessor = header;
  std::string current = body;
  const std::size_t stepLimit = cfg.blocks.size() * 2 + 2;
  for (std::size_t step = 0; step < stepLimit; ++step) {
    const auto block = cfg.blocks.find(current);
    if (block == cfg.blocks.end() || block->second->terminator() == nullptr) {
      return std::nullopt;
    }
    const auto& terminator = *block->second->terminator();
    if (terminator.opcode() != "br") return std::nullopt;
    std::string target;
    if (terminator.operands().size() == 1) {
      target = terminator.operands()[0];
    } else if (terminator.operands().size() == 3) {
      const auto condition = evaluateValue(terminator.operands()[0], predecessor,
                                           definitions, environment);
      if (!condition.has_value()) return std::nullopt;
      target = terminator.operands()[*condition != 0 ? 1 : 2];
    } else {
      return std::nullopt;
    }
    if (target == header) {
      const auto incoming = phiIncomingFrom(accumulatorPhi, current);
      if (!incoming.has_value()) return std::nullopt;
      return resolveAccumulatorChoice(*incoming, current, predecessor,
                                      accumulator, accumulated, definitions,
                                      environment);
    }
    predecessor = current;
    current = target;
  }
  return std::nullopt;
}

std::optional<std::string> classifyTruthTable(const std::array<bool, 4>& table) {
  if (table == std::array<bool, 4>{false, false, false, true}) return "and";
  if (table == std::array<bool, 4>{false, true, true, true}) return "or";
  if (table == std::array<bool, 4>{false, true, true, false}) return "xor";
  return std::nullopt;
}

std::optional<BitAccumulateIdiom> matchBitAccumulate(const Function& function) {
  if ((function.returnType() != "int" && function.returnType() != "i32") ||
      function.parameters().size() != 2 ||
      !isIntegerParameter(function.parameters()[0]) ||
      !isIntegerParameter(function.parameters()[1]) || function.blocks().empty()) {
    return std::nullopt;
  }
  const auto cfg = buildCFG(function);
  if (!cfg.has_value()) return std::nullopt;
  const auto& entry = function.blocks().front();
  const auto* entryTerminator = entry.terminator();
  if (entryTerminator == nullptr || entry.instructions().size() != 1 ||
      entryTerminator->opcode() != "br" ||
      entryTerminator->operands().size() != 1) {
    return std::nullopt;
  }
  const std::string headerName = entryTerminator->operands()[0];
  const auto headerFound = cfg->blocks.find(headerName);
  if (headerFound == cfg->blocks.end()) return std::nullopt;
  const auto& header = *headerFound->second;
  const auto* headerTerminator = header.terminator();
  if (headerTerminator == nullptr || headerTerminator->opcode() != "br" ||
      headerTerminator->operands().size() != 3) {
    return std::nullopt;
  }

  const auto headerPredecessors = cfg->predecessors.find(headerName);
  if (headerPredecessors == cfg->predecessors.end() ||
      headerPredecessors->second.size() != 2) {
    return std::nullopt;
  }
  std::string latchName;
  for (const auto& predecessor : headerPredecessors->second) {
    if (predecessor != entry.name()) {
      if (!latchName.empty()) return std::nullopt;
      latchName = predecessor;
    }
  }
  if (latchName.empty()) return std::nullopt;
  const auto loopBlocks = naturalLoopBlocks(*cfg, headerName, latchName);
  if (loopBlocks.count(entry.name()) != 0) return std::nullopt;

  const std::string firstTarget = headerTerminator->operands()[1];
  const std::string secondTarget = headerTerminator->operands()[2];
  const bool firstInLoop = loopBlocks.count(firstTarget) != 0;
  const bool secondInLoop = loopBlocks.count(secondTarget) != 0;
  if (firstInLoop == secondInLoop) return std::nullopt;
  const std::string bodyName = firstInLoop ? firstTarget : secondTarget;
  const std::string exitName = firstInLoop ? secondTarget : firstTarget;
  const auto exitFound = cfg->blocks.find(exitName);
  if (exitFound == cfg->blocks.end()) return std::nullopt;

  for (const auto& blockName : loopBlocks) {
    const auto block = cfg->blocks.find(blockName);
    if (block == cfg->blocks.end()) return std::nullopt;
    for (const auto& instruction : block->second->instructions()) {
      if (!isAllowedInstruction(instruction)) return std::nullopt;
    }
    if (blockName == headerName) continue;
    const auto successors = cfg->successors.find(blockName);
    if (successors != cfg->successors.end()) {
      for (const auto& successor : successors->second) {
        if (loopBlocks.count(successor) == 0) return std::nullopt;
      }
    }
  }
  const auto reachable = reachableBlocks(*cfg, entry.name());
  for (const auto& blockName : reachable) {
    if (blockName != entry.name() && blockName != exitName &&
        loopBlocks.count(blockName) == 0) {
      return std::nullopt;
    }
  }

  const auto definitions = buildDefinitions(function);
  const auto* lhsPhi = findHeaderPhiInitializedWith(
      header, entry.name(), function.parameters()[0].name);
  const auto* rhsPhi = findHeaderPhiInitializedWith(
      header, entry.name(), function.parameters()[1].name);
  const auto* accumulatorPhi =
      findHeaderPhiInitializedWith(header, entry.name(), "0");
  const auto* weightPhi = findHeaderPhiInitializedWith(header, entry.name(), "1");
  if (lhsPhi == nullptr || rhsPhi == nullptr || accumulatorPhi == nullptr ||
      weightPhi == nullptr) {
    return std::nullopt;
  }
  const Instruction* countPhi = nullptr;
  unsigned iterationCount = 0;
  for (const auto& instruction : header.instructions()) {
    const auto incoming = phiIncomingFrom(instruction, entry.name());
    if (!incoming.has_value()) continue;
    const auto count = parseInteger(*incoming);
    if (!count.has_value() || *count < 2 || *count > 32) continue;
    if (countPhi != nullptr) return std::nullopt;
    countPhi = &instruction;
    iterationCount = static_cast<unsigned>(*count);
  }
  if (countPhi == nullptr) return std::nullopt;
  for (const auto* phi : {lhsPhi, rhsPhi, accumulatorPhi, weightPhi, countPhi}) {
    if (!phiHasExactlyPredecessors(*phi, entry.name(), latchName)) {
      return std::nullopt;
    }
  }

  const auto remainderFor = [&](const Instruction& phi) {
    return findUniqueInstruction(
        function, loopBlocks, [&](const Instruction& instruction) {
          return binaryMatches(instruction, "srem", phi.result(), "2");
        });
  };
  const auto quotientFor = [&](const Instruction& phi) {
    return findUniqueInstruction(
        function, loopBlocks, [&](const Instruction& instruction) {
          return binaryMatches(instruction, "sdiv", phi.result(), "2");
        });
  };
  const auto* lhsRemainder = remainderFor(*lhsPhi);
  const auto* rhsRemainder = remainderFor(*rhsPhi);
  const auto* lhsQuotient = quotientFor(*lhsPhi);
  const auto* rhsQuotient = quotientFor(*rhsPhi);
  const auto* nextWeight = findUniqueInstruction(
      function, loopBlocks, [&](const Instruction& instruction) {
        return binaryMatches(instruction, "mul", weightPhi->result(), "2", true);
      });
  const auto* nextCount = findUniqueInstruction(
      function, loopBlocks, [&](const Instruction& instruction) {
        return binaryMatches(instruction, "sub", countPhi->result(), "1");
      });
  const auto* accumulated = findUniqueInstruction(
      function, loopBlocks, [&](const Instruction& instruction) {
        return binaryMatches(instruction, "add", accumulatorPhi->result(),
                             weightPhi->result(), true);
      });
  if (lhsRemainder == nullptr || rhsRemainder == nullptr ||
      lhsQuotient == nullptr || rhsQuotient == nullptr ||
      nextWeight == nullptr || nextCount == nullptr || accumulated == nullptr) {
    return std::nullopt;
  }
  if (phiIncomingFrom(*lhsPhi, latchName) != lhsQuotient->result() ||
      phiIncomingFrom(*rhsPhi, latchName) != rhsQuotient->result() ||
      phiIncomingFrom(*weightPhi, latchName) != nextWeight->result() ||
      phiIncomingFrom(*countPhi, latchName) != nextCount->result()) {
    return std::nullopt;
  }

  const auto* exitTerminator = exitFound->second->terminator();
  if (exitFound->second->instructions().size() != 1 ||
      exitTerminator == nullptr || exitTerminator->opcode() != "ret" ||
      exitTerminator->operands().size() != 1 ||
      exitTerminator->operands()[0] != accumulatorPhi->result()) {
    return std::nullopt;
  }
  const auto loopCondition = definitions.find(headerTerminator->operands()[0]);
  if (loopCondition == definitions.end() ||
      loopCondition->second.block != headerName ||
      loopCondition->second.instruction->opcode().rfind("icmp.", 0) != 0 ||
      loopCondition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto& conditionOperands = loopCondition->second.instruction->operands();
  if (!((conditionOperands[0] == countPhi->result() &&
         conditionOperands[1] == "0") ||
        (conditionOperands[0] == "0" &&
         conditionOperands[1] == countPhi->result()))) {
    return std::nullopt;
  }
  Environment countZero{{countPhi->result(), 0}};
  Environment countOne{{countPhi->result(), 1}};
  const auto conditionAtZero = evaluateValue(headerTerminator->operands()[0],
                                             entry.name(), definitions, countZero);
  const auto conditionAtOne = evaluateValue(headerTerminator->operands()[0],
                                            entry.name(), definitions, countOne);
  if (!conditionAtZero.has_value() || !conditionAtOne.has_value()) {
    return std::nullopt;
  }
  const auto targetAtZero =
      headerTerminator->operands()[*conditionAtZero != 0 ? 1 : 2];
  const auto targetAtOne =
      headerTerminator->operands()[*conditionAtOne != 0 ? 1 : 2];
  if (targetAtZero != exitName || targetAtOne != bodyName) return std::nullopt;

  std::array<bool, 4> truthTable{};
  for (unsigned lhsBit = 0; lhsBit < 2; ++lhsBit) {
    for (unsigned rhsBit = 0; rhsBit < 2; ++rhsBit) {
      Environment environment{{lhsRemainder->result(), lhsBit},
                              {rhsRemainder->result(), rhsBit}};
      const auto choice = simulateIteration(
          *cfg, definitions, headerName, bodyName, accumulatorPhi->result(),
          accumulated->result(), *accumulatorPhi, environment);
      if (!choice.has_value()) return std::nullopt;
      truthTable[lhsBit * 2 + rhsBit] = *choice;
    }
  }
  const auto opcode = classifyTruthTable(truthTable);
  if (!opcode.has_value()) return std::nullopt;
  return BitAccumulateIdiom{*opcode, headerName, iterationCount};
}

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) names.insert(block.name());
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = stem + "." + std::to_string(index);
    if (names.insert(candidate).second) return candidate;
  }
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = "%" + stem + "." + std::to_string(index);
    if (names.insert(candidate).second) return candidate;
  }
}

void rewriteEntryPhiLabels(Function& function, const std::string& oldEntry,
                           const std::string& fallback) {
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldEntry) operands[index] = fallback;
      }
      rewritten.emplace_back("phi", std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void addNonnegativeFastPath(Function& function,
                            const BitAccumulateIdiom& idiom) {
  const std::string entryName = function.blocks().front().name();
  const std::string fallbackName = freshBlockName(function, "bit.accumulate.fallback");
  const std::string fastName = freshBlockName(function, "bit.accumulate.fast");
  const std::string lhsNonnegative =
      freshValueName(function, "bit.accumulate.lhs.nonnegative");
  const std::string rhsNonnegative =
      freshValueName(function, "bit.accumulate.rhs.nonnegative");
  const std::string bothNonnegative =
      freshValueName(function, "bit.accumulate.both.nonnegative");
  const std::string result = freshValueName(function, "bit.accumulate.result");
  const std::string maskedResult =
      freshValueName(function, "bit.accumulate.masked.result");

  rewriteEntryPhiLabels(function, entryName, fallbackName);
  auto& entryInstructions = function.blocks().front().instructions();
  entryInstructions.clear();
  entryInstructions.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[0].name, "0"},
      lhsNonnegative);
  entryInstructions.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[1].name, "0"},
      rhsNonnegative);
  entryInstructions.emplace_back(
      "and", std::vector<std::string>{lhsNonnegative, rhsNonnegative},
      bothNonnegative);
  entryInstructions.emplace_back(
      "br", std::vector<std::string>{bothNonnegative, fastName, fallbackName});

  BasicBlock fallback{fallbackName};
  fallback.addInstruction(Instruction{"br", {idiom.fallbackTarget}});
  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{
      idiom.opcode,
      {function.parameters()[0].name, function.parameters()[1].name}, result});
  if (idiom.iterationCount < 32) {
    const std::uint32_t mask =
        (std::uint32_t{1} << idiom.iterationCount) - std::uint32_t{1};
    fast.addInstruction(
        Instruction{"and", {result, std::to_string(mask)}, maskedResult});
    fast.addInstruction(Instruction{"ret", {maskedResult}});
  } else {
    fast.addInstruction(Instruction{"ret", {result}});
  }
  function.blocks().insert(function.blocks().begin() + 1, std::move(fast));
  function.blocks().insert(function.blocks().begin() + 2, std::move(fallback));
}

void replaceWithSignedClosedForm(Function& function,
                                 const BitAccumulateIdiom& idiom) {
  const std::string entryName = function.blocks().front().name();
  const std::string lhs = function.parameters()[0].name;
  const std::string rhs = function.parameters()[1].name;
  const std::string result =
      freshValueName(function, "bit.accumulate.closed.result");
  const std::string maskedResult =
      freshValueName(function, "bit.accumulate.closed.masked.result");
  BasicBlock entry{entryName};

  if (idiom.opcode == "and" || idiom.opcode == "or") {
    const std::string lhsNonnegative =
        freshValueName(function, "bit.accumulate.closed.lhs.nonnegative");
    const std::string rhsNonnegative =
        freshValueName(function, "bit.accumulate.closed.rhs.nonnegative");
    const std::string normalizedLhs =
        freshValueName(function, "bit.accumulate.closed.lhs");
    const std::string normalizedRhs =
        freshValueName(function, "bit.accumulate.closed.rhs");
    entry.addInstruction(
        Instruction{"icmp.sge", {lhs, "0"}, lhsNonnegative});
    entry.addInstruction(
        Instruction{"icmp.sge", {rhs, "0"}, rhsNonnegative});
    entry.addInstruction(
        Instruction{"select", {lhsNonnegative, lhs, "0"}, normalizedLhs});
    entry.addInstruction(
        Instruction{"select", {rhsNonnegative, rhs, "0"}, normalizedRhs});
    entry.addInstruction(
        Instruction{idiom.opcode, {normalizedLhs, normalizedRhs}, result});
  } else {
    const std::string lhsNegative =
        freshValueName(function, "bit.accumulate.closed.lhs.negative");
    const std::string rhsNegative =
        freshValueName(function, "bit.accumulate.closed.rhs.negative");
    const std::string negatedLhs =
        freshValueName(function, "bit.accumulate.closed.lhs.negated");
    const std::string negatedRhs =
        freshValueName(function, "bit.accumulate.closed.rhs.negated");
    const std::string magnitudeLhs =
        freshValueName(function, "bit.accumulate.closed.lhs.magnitude");
    const std::string magnitudeRhs =
        freshValueName(function, "bit.accumulate.closed.rhs.magnitude");
    const std::string signsDiffer =
        freshValueName(function, "bit.accumulate.closed.signs.differ");
    const std::string sameSignResult =
        freshValueName(function, "bit.accumulate.closed.same.sign");
    const std::string differentSignResult =
        freshValueName(function, "bit.accumulate.closed.different.sign");
    entry.addInstruction(Instruction{"icmp.slt", {lhs, "0"}, lhsNegative});
    entry.addInstruction(Instruction{"icmp.slt", {rhs, "0"}, rhsNegative});
    entry.addInstruction(Instruction{"sub", {"0", lhs}, negatedLhs});
    entry.addInstruction(Instruction{"sub", {"0", rhs}, negatedRhs});
    entry.addInstruction(
        Instruction{"select", {lhsNegative, negatedLhs, lhs}, magnitudeLhs});
    entry.addInstruction(
        Instruction{"select", {rhsNegative, negatedRhs, rhs}, magnitudeRhs});
    entry.addInstruction(
        Instruction{"xor", {lhsNegative, rhsNegative}, signsDiffer});
    entry.addInstruction(
        Instruction{"xor", {magnitudeLhs, magnitudeRhs}, sameSignResult});
    entry.addInstruction(
        Instruction{"or", {magnitudeLhs, magnitudeRhs}, differentSignResult});
    entry.addInstruction(Instruction{
        "select", {signsDiffer, differentSignResult, sameSignResult}, result});
  }

  if (idiom.iterationCount < 32) {
    const std::uint32_t mask =
        (std::uint32_t{1} << idiom.iterationCount) - std::uint32_t{1};
    entry.addInstruction(
        Instruction{"and", {result, std::to_string(mask)}, maskedResult});
    entry.addInstruction(Instruction{"ret", {maskedResult}});
  } else {
    entry.addInstruction(Instruction{"ret", {result}});
  }
  function.blocks().clear();
  function.addBlock(std::move(entry));
}

}  // namespace

void BitwiseIdiomRecognitionPass::run(compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_BIT_ACCUMULATE_RECOGNITION") != nullptr) {
    return;
  }
  for (auto& function : module.functions()) {
    const auto idiom = matchBitAccumulate(function);
    if (!idiom.has_value()) continue;
    if (std::getenv("COMPILER_DISABLE_SIGNED_BITWISE_CLOSED_FORM") != nullptr ||
        std::getenv("COMPILER_DISABLE_CLOSED_HELPER_STATE_PIPELINE") != nullptr) {
      addNonnegativeFastPath(function, *idiom);
    } else {
      replaceWithSignedClosedForm(function, *idiom);
    }
  }
}

}  // namespace compiler::midend::pass
