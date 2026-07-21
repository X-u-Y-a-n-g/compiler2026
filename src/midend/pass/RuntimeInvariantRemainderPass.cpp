#include "../../../include/compiler/midend/pass/RuntimeInvariantRemainderPass.h"

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

#include "../../../include/compiler/ir/core/Constant.h"
#include "../../../include/compiler/ir/core/GlobalVariable.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Constant;
using compiler::ir::core::Function;
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr std::uint64_t kReciprocalScale = std::uint64_t{1} << 32;

struct StoreSite {
  std::size_t function = 0;
  std::size_t block = 0;
  std::size_t instruction = 0;
  std::string value;
};

struct Candidate {
  std::string symbol;
  std::size_t global = 0;
  StoreSite store;
  std::size_t hotRemainders = 0;
};

struct RemainderSite {
  std::size_t function = 0;
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct RemainderLeaf {
  std::string function;
  std::vector<std::string> parameters;
  std::size_t numeratorParameter = 0;
};

bool isI32(const std::string &type) { return type == "int" || type == "i32"; }

std::unordered_set<std::string> cyclicBlocks(const Function &function) {
  const std::size_t blockCount = function.blocks().size();
  if (blockCount == 0 || blockCount > 512)
    return {};
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < blockCount; ++i) {
    index.emplace(function.blocks()[i].name(), i);
  }
  std::vector<std::vector<bool>> reachable(
      blockCount, std::vector<bool>(blockCount, false));
  for (std::size_t i = 0; i < blockCount; ++i) {
    const auto *terminator = function.blocks()[i].terminator();
    if (terminator == nullptr || terminator->opcode() != "br")
      continue;
    const auto &operands = terminator->operands();
    const std::size_t firstTarget = operands.size() == 1 ? 0 : 1;
    for (std::size_t operand = firstTarget; operand < operands.size();
         ++operand) {
      const auto found = index.find(operands[operand]);
      if (found != index.end())
        reachable[i][found->second] = true;
    }
  }
  for (std::size_t through = 0; through < blockCount; ++through) {
    for (std::size_t from = 0; from < blockCount; ++from) {
      if (!reachable[from][through])
        continue;
      for (std::size_t to = 0; to < blockCount; ++to) {
        reachable[from][to] = reachable[from][to] || reachable[through][to];
      }
    }
  }
  std::unordered_set<std::string> cyclic;
  for (std::size_t i = 0; i < blockCount; ++i) {
    if (reachable[i][i])
      cyclic.insert(function.blocks()[i].name());
  }
  return cyclic;
}

std::unordered_set<std::string> hotFunctions(const Module &module) {
  std::unordered_set<std::string> hot;
  std::unordered_map<std::string, std::vector<std::string>> calls;
  for (const auto &function : module.functions()) {
    const auto cyclic = cyclicBlocks(function);
    auto &callees = calls[function.name()];
    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() != "call" || instruction.operands().empty() ||
            instruction.operands().front().size() < 2 ||
            instruction.operands().front().front() != '@') {
          continue;
        }
        const std::string callee = instruction.operands().front().substr(1);
        callees.push_back(callee);
        if (cyclic.find(block.name()) != cyclic.end())
          hot.insert(callee);
      }
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    std::vector<std::string> newlyHot;
    for (const auto &function : hot) {
      const auto found = calls.find(function);
      if (found == calls.end())
        continue;
      for (const auto &callee : found->second) {
        if (hot.find(callee) == hot.end())
          newlyHot.push_back(callee);
      }
    }
    for (const auto &function : newlyHot) {
      changed |= hot.insert(function).second;
    }
  }
  return hot;
}

std::optional<std::int64_t> scalarInitializer(const GlobalVariable &global) {
  if (!global.initializer().isScalar())
    return std::nullopt;
  long long value = 0;
  if (!detail::parseIntegerLiteral(global.initializer().literal(), value) ||
      value < INT32_MIN || value > INT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(value);
}

std::unordered_set<std::string> collectValueNames(const Function &function) {
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

std::unordered_set<std::string> collectBlockNames(const Function &function) {
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

std::vector<std::string> successors(const Instruction &terminator) {
  if (terminator.opcode() != "br")
    return {};
  if (terminator.operands().size() == 1)
    return {terminator.operands()[0]};
  if (terminator.operands().size() == 3) {
    return {terminator.operands()[1], terminator.operands()[2]};
  }
  return {};
}

void replacePhiPredecessor(Function &function,
                           const std::vector<std::string> &successorNames,
                           const std::string &oldPredecessor,
                           const std::string &newPredecessor) {
  std::unordered_set<std::string> successorsSet(successorNames.begin(),
                                                successorNames.end());
  for (auto &block : function.blocks()) {
    if (successorsSet.find(block.name()) == successorsSet.end())
      continue;
    for (auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi")
        break;
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldPredecessor) {
          operands[index] = newPredecessor;
        }
      }
      instruction =
          Instruction{"phi", std::move(operands), instruction.result()};
    }
  }
}

std::optional<Candidate> selectCandidate(const Module &module) {
  const auto hot = hotFunctions(module);
  std::optional<Candidate> best;
  for (std::size_t globalIndex = 0; globalIndex < module.globals().size();
       ++globalIndex) {
    const auto &global = module.globals()[globalIndex];
    if (global.isConst() || !isI32(global.type()) ||
        !scalarInitializer(global).has_value()) {
      continue;
    }
    const std::string symbol = "@" + global.name();
    bool valid = true;
    std::vector<StoreSite> stores;
    std::size_t hotRemainders = 0;
    for (std::size_t functionIndex = 0;
         functionIndex < module.functions().size(); ++functionIndex) {
      const auto &function = module.functions()[functionIndex];
      const auto cyclic = cyclicBlocks(function);
      std::unordered_set<std::string> loads;
      for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
           ++blockIndex) {
        const auto &block = function.blocks()[blockIndex];
        for (std::size_t instructionIndex = 0;
             instructionIndex < block.instructions().size();
             ++instructionIndex) {
          const auto &instruction = block.instructions()[instructionIndex];
          const auto &operands = instruction.operands();
          bool directAccess = false;
          if (instruction.opcode() == "load" && instruction.hasResult() &&
              operands.size() == 2 && operands[1] == symbol) {
            loads.insert(instruction.result());
            directAccess = true;
          } else if (instruction.opcode() == "store" && operands.size() == 3 &&
                     operands[2] == symbol && isI32(operands[0])) {
            stores.push_back(StoreSite{functionIndex, blockIndex,
                                       instructionIndex, operands[1]});
            directAccess = true;
            if (cyclic.find(block.name()) != cyclic.end())
              valid = false;
          }
          if (!directAccess && std::find(operands.begin(), operands.end(),
                                         symbol) != operands.end()) {
            valid = false;
          }
          if (instruction.opcode() == "srem" && operands.size() == 2 &&
              loads.find(operands[1]) != loads.end() &&
              hot.find(function.name()) != hot.end()) {
            ++hotRemainders;
          }
        }
      }
    }
    if (!valid || stores.size() != 1 || hotRemainders == 0)
      continue;
    Candidate candidate{symbol, globalIndex, stores.front(), hotRemainders};
    if (!best.has_value() || candidate.hotRemainders > best->hotRemainders) {
      best = std::move(candidate);
    }
  }
  return best;
}

std::string addMagicGlobal(Module &module, const Candidate &candidate) {
  std::unordered_set<std::string> names;
  for (const auto &global : module.globals())
    names.insert(global.name());
  std::string name;
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidateName =
        "runtime.remainder.magic." + std::to_string(suffix);
    if (names.insert(candidateName).second) {
      name = candidateName;
      break;
    }
  }
  const auto initial = scalarInitializer(module.globals()[candidate.global]);
  std::uint64_t reciprocal = 0;
  if (initial.has_value() && *initial > 0) {
    reciprocal = kReciprocalScale / static_cast<std::uint64_t>(*initial);
  }
  module.addGlobal(GlobalVariable{
      name, "long", Constant::scalar("long", std::to_string(reciprocal))});
  return "@" + name;
}

std::vector<RemainderLeaf> collectRemainderLeaves(const Module &module,
                                                  const std::string &symbol) {
  std::vector<RemainderLeaf> leaves;
  for (const auto &function : module.functions()) {
    if (!isI32(function.returnType()) || function.blocks().size() != 1) {
      continue;
    }
    const auto &instructions = function.blocks().front().instructions();
    if (instructions.size() != 3 || instructions[0].opcode() != "load" ||
        !instructions[0].hasResult() ||
        instructions[0].operands().size() != 2 ||
        instructions[0].operands()[1] != symbol ||
        instructions[1].opcode() != "srem" || !instructions[1].hasResult() ||
        instructions[1].operands().size() != 2 ||
        instructions[1].operands()[1] != instructions[0].result() ||
        instructions[2].opcode() != "ret" ||
        instructions[2].operands().size() != 1 ||
        instructions[2].operands()[0] != instructions[1].result()) {
      continue;
    }
    std::size_t numeratorParameter = function.parameters().size();
    for (std::size_t index = 0; index < function.parameters().size(); ++index) {
      if (isI32(function.parameters()[index].type) &&
          function.parameters()[index].name == instructions[1].operands()[0]) {
        numeratorParameter = index;
        break;
      }
    }
    if (numeratorParameter == function.parameters().size())
      continue;
    RemainderLeaf leaf;
    leaf.function = function.name();
    leaf.numeratorParameter = numeratorParameter;
    for (const auto &parameter : function.parameters()) {
      leaf.parameters.push_back(parameter.name);
    }
    leaves.push_back(std::move(leaf));
  }
  return leaves;
}

void inlineRemainderLeafCalls(
    Module &module, const std::string &symbol,
    const std::unordered_set<std::string> &hotCallers) {
  const auto leaves = collectRemainderLeaves(module, symbol);
  if (leaves.empty())
    return;
  std::unordered_map<std::string, RemainderLeaf> leafBySymbol;
  for (const auto &leaf : leaves) {
    leafBySymbol.emplace("@" + leaf.function, leaf);
  }
  std::unordered_set<std::string> inlined;
  for (auto &function : module.functions()) {
    if (hotCallers.find(function.name()) == hotCallers.end())
      continue;
    auto values = collectValueNames(function);
    for (auto &block : function.blocks()) {
      std::vector<Instruction> rewritten;
      rewritten.reserve(block.instructions().size() + 2);
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() != "call" || !instruction.hasResult() ||
            instruction.operands().empty()) {
          rewritten.push_back(instruction);
          continue;
        }
        const auto found = leafBySymbol.find(instruction.operands().front());
        if (found == leafBySymbol.end() ||
            instruction.operands().size() !=
                found->second.parameters.size() + 1) {
          rewritten.push_back(instruction);
          continue;
        }
        const std::string divisor =
            freshValue(values, "runtime.remainder.inline.divisor");
        rewritten.emplace_back("load", std::vector<std::string>{"int", symbol},
                               divisor);
        rewritten.emplace_back(
            "srem",
            std::vector<std::string>{
                instruction.operands()[found->second.numeratorParameter + 1],
                divisor},
            instruction.result());
        inlined.insert(found->second.function);
      }
      block.instructions() = std::move(rewritten);
    }
  }
  if (inlined.empty())
    return;
  std::unordered_set<std::string> stillCalled;
  for (const auto &function : module.functions()) {
    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() == "call" && !instruction.operands().empty() &&
            instruction.operands().front().size() > 1 &&
            instruction.operands().front().front() == '@') {
          stillCalled.insert(instruction.operands().front().substr(1));
        }
      }
    }
  }
  module.functions().erase(
      std::remove_if(module.functions().begin(), module.functions().end(),
                     [&](const Function &function) {
                       return inlined.find(function.name()) != inlined.end() &&
                              stillCalled.find(function.name()) ==
                                  stillCalled.end();
                     }),
      module.functions().end());
}

void instrumentStore(Module &module, const StoreSite &site,
                     const std::string &magicSymbol) {
  auto &function = module.functions()[site.function];
  auto values = collectValueNames(function);
  auto blocks = collectBlockNames(function);
  const std::string originalName = function.blocks()[site.block].name();
  const std::string calculateName =
      freshBlock(blocks, "runtime.remainder.calculate");
  const std::string continuationName =
      freshBlock(blocks, "runtime.remainder.store.continuation");
  const std::string positive = freshValue(values, "runtime.remainder.positive");
  const std::string divisor64 =
      freshValue(values, "runtime.remainder.divisor64");
  const std::string reciprocal =
      freshValue(values, "runtime.remainder.reciprocal");

  auto &instructions = function.blocks()[site.block].instructions();
  std::vector<Instruction> suffix;
  suffix.reserve(instructions.size() - site.instruction - 1);
  for (std::size_t index = site.instruction + 1; index < instructions.size();
       ++index) {
    suffix.push_back(std::move(instructions[index]));
  }
  std::vector<std::string> oldSuccessors;
  if (!suffix.empty() && suffix.back().isTerminator()) {
    oldSuccessors = successors(suffix.back());
  }
  instructions.erase(instructions.begin() +
                         static_cast<std::ptrdiff_t>(site.instruction + 1),
                     instructions.end());
  instructions.emplace_back(
      "icmp.sgt", std::vector<std::string>{site.value, "0"}, positive);
  instructions.emplace_back(
      "br",
      std::vector<std::string>{positive, calculateName, continuationName});

  BasicBlock calculate{calculateName};
  calculate.addInstruction(
      Instruction{"zext", {site.value, "long"}, divisor64});
  calculate.addInstruction(Instruction{
      "sdiv", {std::to_string(kReciprocalScale), divisor64}, reciprocal});
  calculate.addInstruction(
      Instruction{"store", {"long", reciprocal, magicSymbol}});
  calculate.addInstruction(Instruction{"br", {continuationName}});

  BasicBlock continuation{continuationName};
  for (auto &instruction : suffix) {
    continuation.addInstruction(std::move(instruction));
  }
  replacePhiPredecessor(function, oldSuccessors, originalName,
                        continuationName);
  auto insert =
      function.blocks().begin() + static_cast<std::ptrdiff_t>(site.block + 1);
  insert = function.blocks().insert(insert, std::move(calculate));
  ++insert;
  function.blocks().insert(insert, std::move(continuation));
}

std::vector<RemainderSite>
collectRemainderSites(const Module &module, const std::string &symbol,
                      const std::unordered_set<std::string> &hot) {
  std::vector<RemainderSite> sites;
  for (std::size_t functionIndex = 0; functionIndex < module.functions().size();
       ++functionIndex) {
    const auto &function = module.functions()[functionIndex];
    if (hot.find(function.name()) == hot.end())
      continue;
    std::unordered_set<std::string> loads;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      const auto &block = function.blocks()[blockIndex];
      for (std::size_t instructionIndex = 0;
           instructionIndex < block.instructions().size(); ++instructionIndex) {
        const auto &instruction = block.instructions()[instructionIndex];
        if (instruction.opcode() == "load" && instruction.hasResult() &&
            instruction.operands().size() == 2 &&
            instruction.operands()[1] == symbol) {
          loads.insert(instruction.result());
        }
        if (instruction.opcode() == "srem" &&
            instruction.operands().size() == 2 &&
            loads.find(instruction.operands()[1]) != loads.end()) {
          sites.push_back(
              RemainderSite{functionIndex, blockIndex, instructionIndex});
        }
      }
    }
  }
  std::sort(sites.begin(), sites.end(),
            [](const RemainderSite &lhs, const RemainderSite &rhs) {
              if (lhs.function != rhs.function)
                return lhs.function > rhs.function;
              if (lhs.block != rhs.block)
                return lhs.block > rhs.block;
              return lhs.instruction > rhs.instruction;
            });
  return sites;
}

void rewriteRemainder(Function &function, const RemainderSite &site,
                      const std::string &magicSymbol) {
  auto values = collectValueNames(function);
  auto blocks = collectBlockNames(function);
  const auto original =
      function.blocks()[site.block].instructions()[site.instruction];
  const std::string originalName = function.blocks()[site.block].name();
  const std::string fastName = freshBlock(blocks, "runtime.remainder.fast");
  const std::string fallbackName =
      freshBlock(blocks, "runtime.remainder.fallback");
  const std::string continuationName =
      freshBlock(blocks, "runtime.remainder.continuation");
  const std::string positiveDivisor =
      freshValue(values, "runtime.remainder.positive.divisor");
  const std::string nonnegativeNumerator =
      freshValue(values, "runtime.remainder.nonnegative.numerator");
  const std::string eligible = freshValue(values, "runtime.remainder.eligible");
  const std::string numerator64 =
      freshValue(values, "runtime.remainder.numerator64");
  const std::string reciprocal =
      freshValue(values, "runtime.remainder.reciprocal.load");
  const std::string product = freshValue(values, "runtime.remainder.product");
  const std::string quotient64 =
      freshValue(values, "runtime.remainder.quotient64");
  const std::string quotient = freshValue(values, "runtime.remainder.quotient");
  const std::string quotientProduct =
      freshValue(values, "runtime.remainder.quotient.product");
  const std::string preliminary =
      freshValue(values, "runtime.remainder.preliminary");
  const std::string corrected =
      freshValue(values, "runtime.remainder.corrected");
  const std::string needsCorrection =
      freshValue(values, "runtime.remainder.needs.correction");
  const std::string fastResult =
      freshValue(values, "runtime.remainder.fast.result");
  const std::string fallbackResult =
      freshValue(values, "runtime.remainder.fallback.result");

  auto &instructions = function.blocks()[site.block].instructions();
  std::vector<Instruction> suffix;
  suffix.reserve(instructions.size() - site.instruction - 1);
  for (std::size_t index = site.instruction + 1; index < instructions.size();
       ++index) {
    suffix.push_back(std::move(instructions[index]));
  }
  std::vector<std::string> oldSuccessors;
  if (!suffix.empty() && suffix.back().isTerminator()) {
    oldSuccessors = successors(suffix.back());
  }
  instructions.erase(instructions.begin() +
                         static_cast<std::ptrdiff_t>(site.instruction),
                     instructions.end());
  instructions.emplace_back(
      "icmp.sgt", std::vector<std::string>{original.operands()[1], "0"},
      positiveDivisor);
  instructions.emplace_back(
      "icmp.sge", std::vector<std::string>{original.operands()[0], "0"},
      nonnegativeNumerator);
  instructions.emplace_back(
      "and", std::vector<std::string>{positiveDivisor, nonnegativeNumerator},
      eligible);
  instructions.emplace_back(
      "br", std::vector<std::string>{eligible, fastName, fallbackName});

  BasicBlock fast{fastName};
  fast.addInstruction(
      Instruction{"zext", {original.operands()[0], "long"}, numerator64});
  fast.addInstruction(Instruction{"load", {"long", magicSymbol}, reciprocal});
  fast.addInstruction(Instruction{"mul", {numerator64, reciprocal}, product});
  fast.addInstruction(Instruction{"lshr", {product, "32"}, quotient64});
  fast.addInstruction(Instruction{"trunc", {quotient64, "int"}, quotient});
  fast.addInstruction(
      Instruction{"mul", {quotient, original.operands()[1]}, quotientProduct});
  fast.addInstruction(Instruction{
      "sub", {original.operands()[0], quotientProduct}, preliminary});
  fast.addInstruction(
      Instruction{"sub", {preliminary, original.operands()[1]}, corrected});
  fast.addInstruction(Instruction{
      "icmp.sge", {preliminary, original.operands()[1]}, needsCorrection});
  fast.addInstruction(Instruction{
      "select", {needsCorrection, corrected, preliminary}, fastResult});
  fast.addInstruction(Instruction{"br", {continuationName}});

  BasicBlock fallback{fallbackName};
  fallback.addInstruction(
      Instruction{"srem", original.operands(), fallbackResult});
  fallback.addInstruction(Instruction{"br", {continuationName}});

  BasicBlock continuation{continuationName};
  continuation.addInstruction(
      Instruction{"phi",
                  {"int", fastResult, fastName, fallbackResult, fallbackName},
                  original.result()});
  for (auto &instruction : suffix) {
    continuation.addInstruction(std::move(instruction));
  }
  replacePhiPredecessor(function, oldSuccessors, originalName,
                        continuationName);
  auto insert =
      function.blocks().begin() + static_cast<std::ptrdiff_t>(site.block + 1);
  insert = function.blocks().insert(insert, std::move(fast));
  ++insert;
  insert = function.blocks().insert(insert, std::move(fallback));
  ++insert;
  function.blocks().insert(insert, std::move(continuation));
}

} // namespace

void RuntimeInvariantRemainderPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_RUNTIME_INVARIANT_REMAINDER") != nullptr) {
    return;
  }
  const auto candidate = selectCandidate(module);
  if (!candidate.has_value())
    return;
  const auto initiallyHot = hotFunctions(module);
  const std::string magicSymbol = addMagicGlobal(module, *candidate);
  instrumentStore(module, candidate->store, magicSymbol);
  inlineRemainderLeafCalls(module, candidate->symbol, initiallyHot);
  const auto hot = hotFunctions(module);
  const auto sites = collectRemainderSites(module, candidate->symbol, hot);
  for (const auto &site : sites) {
    rewriteRemainder(module.functions()[site.function], site, magicSymbol);
  }
}

} // namespace compiler::midend::pass
