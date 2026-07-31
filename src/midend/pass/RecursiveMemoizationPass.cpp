#include "../../../include/compiler/midend/pass/RecursiveMemoizationPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/core/Constant.h"
#include "../../../include/compiler/ir/core/GlobalVariable.h"
#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Constant;
using compiler::ir::core::Function;
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;
using compiler::midend::analysis::FunctionEffectAnalysis;

constexpr std::size_t kMaximumParameters = 3;
constexpr std::size_t kModuleCacheBudgetBytes = 512U * 1024U;
constexpr std::size_t kReadOnlyLoopCacheBudgetBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMinimumCacheEntries = 1024U;
struct CallSite {
  std::size_t caller = 0;
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Candidate {
  std::size_t function = 0;
  std::size_t capacity = 0;
  bool readOnlyLoopLifetime = false;
  bool twoProbeLoopCache = false;
};

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
  if (terminator->operands().size() == 1)
    return {terminator->operands()[0]};
  if (terminator->operands().size() == 3)
    return {terminator->operands()[1], terminator->operands()[2]};
  return {};
}

bool blockBelongsToCycle(const Function &function, std::size_t target) {
  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t block = 0; block < function.blocks().size(); ++block)
    indices.emplace(function.blocks()[block].name(), block);
  std::vector<std::vector<std::size_t>> successors(function.blocks().size());
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto &name : branchTargets(function.blocks()[block])) {
      const auto found = indices.find(name);
      if (found != indices.end())
        successors[block].push_back(found->second);
    }
  }
  std::deque<std::size_t> worklist;
  std::unordered_set<std::size_t> visited;
  for (const auto successor : successors[target])
    worklist.push_back(successor);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    if (block == target)
      return true;
    if (!visited.insert(block).second)
      continue;
    for (const auto successor : successors[block])
      worklist.push_back(successor);
  }
  return false;
}

std::size_t floorPowerOfTwo(std::size_t value) {
  std::size_t result = 1;
  while (result <= value / 2)
    result *= 2;
  return result;
}

std::size_t cacheCapacity(std::size_t parameters, std::size_t budgetBytes) {
  const auto bytesPerEntry = (parameters + 2U) * sizeof(std::int32_t);
  const auto maximum = budgetBytes / bytesPerEntry;
  if (maximum < kMinimumCacheEntries)
    return 0;
  return floorPowerOfTwo(maximum);
}

std::vector<CallSite> callsTo(const Module &module, const std::string &callee) {
  std::vector<CallSite> result;
  for (std::size_t caller = 0; caller < module.functions().size(); ++caller) {
    const auto &function = module.functions()[caller];
    for (std::size_t block = 0; block < function.blocks().size(); ++block) {
      const auto &instructions = function.blocks()[block].instructions();
      for (std::size_t instruction = 0; instruction < instructions.size();
           ++instruction) {
        if (instructions[instruction].opcode() == "call" &&
            !instructions[instruction].operands().empty() &&
            instructions[instruction].operands()[0] == callee)
          result.push_back({caller, block, instruction});
      }
    }
  }
  return result;
}

bool entryHasPhi(const Function &function) {
  if (function.blocks().empty())
    return true;
  for (const auto &instruction : function.blocks().front().instructions()) {
    if (instruction.opcode() == "phi")
      return true;
  }
  return false;
}

std::optional<Candidate> findCandidate(const Module &module,
                                       std::size_t functionIndex,
                                       const FunctionEffectAnalysis &effects) {
  const auto &function = module.functions()[functionIndex];
  if (function.returnType() != "int" || function.parameters().empty() ||
      function.parameters().size() > kMaximumParameters ||
      function.blocks().empty() || entryHasPhi(function))
    return std::nullopt;
  for (const auto &parameter : function.parameters()) {
    if (parameter.type != "int")
      return std::nullopt;
  }
  const auto &effect = effects.effectFor(function.name());
  if (effect.writesExternalMemory || effect.hasOtherSideEffects)
    return std::nullopt;

  const auto allCalls = callsTo(module, "@" + function.name());
  std::size_t recursiveCalls = 0;
  std::vector<CallSite> externalCalls;
  for (const auto &call : allCalls) {
    if (call.caller == functionIndex)
      ++recursiveCalls;
    else
      externalCalls.push_back(call);
  }
  if (recursiveCalls < 2 || externalCalls.empty())
    return std::nullopt;
  if (effect.readsExternalMemory) {
    if (externalCalls.size() != 1)
      return std::nullopt;
    const auto &root = externalCalls.front();
    const auto &caller = module.functions()[root.caller];
    if (caller.name() != "main" || blockBelongsToCycle(caller, root.block))
      return std::nullopt;
  }
  return Candidate{functionIndex, 0};
}

using DefinitionMap =
    std::unordered_map<std::string, const Instruction*>;

DefinitionMap definitionsFor(const Function& function) {
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

std::string pointerRoot(const std::string& pointer,
                        const DefinitionMap& definitions) {
  std::string current = pointer;
  std::unordered_set<std::string> visited;
  while (visited.insert(current).second) {
    const auto found = definitions.find(current);
    if (found == definitions.end()) {
      return current;
    }
    const auto* instruction = found->second;
    if ((instruction->opcode() != "getelementptr" &&
         instruction->opcode() != "getelementptr.linear" &&
         instruction->opcode() != "gep" &&
         instruction->opcode() != "gep.linear" &&
         instruction->opcode() != "cast") ||
        instruction->operands().empty()) {
      return current;
    }
    current = instruction->operands().front();
  }
  return {};
}

bool collectReadGlobals(
    const Module& module, std::size_t functionIndex,
    const std::unordered_map<std::string, std::size_t>& functionIndices,
    std::unordered_set<std::size_t>& visiting,
    std::unordered_map<std::size_t, std::unordered_set<std::string>>& cache,
    std::unordered_set<std::string>& result) {
  const auto cached = cache.find(functionIndex);
  if (cached != cache.end()) {
    result.insert(cached->second.begin(), cached->second.end());
    return true;
  }
  if (!visiting.insert(functionIndex).second) {
    return false;
  }
  const auto& function = module.functions()[functionIndex];
  const auto definitions = definitionsFor(function);
  std::unordered_set<std::string> localAllocas;
  for (const auto& [value, definition] : definitions) {
    if (definition->opcode() == "alloca") {
      localAllocas.insert(value);
    }
  }
  std::unordered_set<std::string> reads;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "load") {
        if (instruction.operands().empty()) {
          visiting.erase(functionIndex);
          return false;
        }
        const auto root =
            pointerRoot(instruction.operands().back(), definitions);
        if (!root.empty() && root.front() == '@') {
          reads.insert(root);
        } else if (localAllocas.count(root) == 0) {
          visiting.erase(functionIndex);
          return false;
        }
        continue;
      }
      if (instruction.opcode() != "call" ||
          instruction.operands().empty()) {
        continue;
      }
      const auto& symbol = instruction.operands().front();
      if (symbol.size() < 2 || symbol.front() != '@') {
        visiting.erase(functionIndex);
        return false;
      }
      const auto callee = functionIndices.find(symbol.substr(1));
      if (callee == functionIndices.end() ||
          !collectReadGlobals(module, callee->second, functionIndices,
                              visiting, cache, reads)) {
        visiting.erase(functionIndex);
        return false;
      }
    }
  }
  visiting.erase(functionIndex);
  cache.emplace(functionIndex, reads);
  result.insert(reads.begin(), reads.end());
  return true;
}

std::unordered_set<std::size_t> stronglyConnectedBlocks(
    const Function& function, std::size_t seed) {
  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    indices.emplace(function.blocks()[block].name(), block);
  }
  std::vector<std::vector<std::size_t>> successors(function.blocks().size());
  std::vector<std::vector<std::size_t>> predecessors(function.blocks().size());
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& target : branchTargets(function.blocks()[block])) {
      const auto found = indices.find(target);
      if (found == indices.end()) {
        continue;
      }
      successors[block].push_back(found->second);
      predecessors[found->second].push_back(block);
    }
  }
  const auto reachable = [&](const auto& edges) {
    std::unordered_set<std::size_t> visited;
    std::deque<std::size_t> worklist{seed};
    while (!worklist.empty()) {
      const auto block = worklist.front();
      worklist.pop_front();
      if (!visited.insert(block).second) {
        continue;
      }
      for (const auto next : edges[block]) {
        worklist.push_back(next);
      }
    }
    return visited;
  };
  const auto forward = reachable(successors);
  const auto backward = reachable(predecessors);
  std::unordered_set<std::size_t> region;
  for (const auto block : forward) {
    if (backward.count(block) != 0) {
      region.insert(block);
    }
  }
  return region;
}

std::optional<Candidate> findReadOnlyLoopCandidate(
    const Module& module, std::size_t functionIndex,
    const FunctionEffectAnalysis& effects) {
  const auto& function = module.functions()[functionIndex];
  if (function.returnType() != "int" || function.parameters().empty() ||
      function.parameters().size() > kMaximumParameters ||
      function.blocks().empty() || entryHasPhi(function) ||
      !effects.effectFor(function.name()).isReadOnly()) {
    return std::nullopt;
  }
  for (const auto& parameter : function.parameters()) {
    if (parameter.type != "int") {
      return std::nullopt;
    }
  }
  bool functionHasCycle = false;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    functionHasCycle |= blockBelongsToCycle(function, block);
  }
  if (!functionHasCycle) {
    return std::nullopt;
  }

  const auto calls = callsTo(module, "@" + function.name());
  if (calls.size() != 1 || calls.front().caller == functionIndex) {
    return std::nullopt;
  }
  const auto& call = calls.front();
  const auto& caller = module.functions()[call.caller];
  if (caller.name() != "main" || !blockBelongsToCycle(caller, call.block)) {
    return std::nullopt;
  }

  std::unordered_map<std::string, std::size_t> functionIndices;
  for (std::size_t index = 0; index < module.functions().size(); ++index) {
    functionIndices.emplace(module.functions()[index].name(), index);
  }
  std::unordered_set<std::size_t> visiting;
  std::unordered_map<std::size_t, std::unordered_set<std::string>> cache;
  std::unordered_set<std::string> readGlobals;
  if (!collectReadGlobals(module, functionIndex, functionIndices, visiting,
                          cache, readGlobals) ||
      readGlobals.empty()) {
    return std::nullopt;
  }

  const auto loopBlocks = stronglyConnectedBlocks(caller, call.block);
  const auto definitions = definitionsFor(caller);
  std::unordered_set<std::string> localAllocas;
  for (const auto& [value, definition] : definitions) {
    if (definition->opcode() == "alloca") {
      localAllocas.insert(value);
    }
  }
  for (const auto blockIndex : loopBlocks) {
    for (const auto& instruction :
         caller.blocks()[blockIndex].instructions()) {
      if (instruction.opcode() == "store") {
        if (instruction.operands().empty()) {
          return std::nullopt;
        }
        const auto root =
            pointerRoot(instruction.operands().back(), definitions);
        if ((!root.empty() && root.front() == '@' &&
             readGlobals.count(root) != 0) ||
            (root.empty() ||
             (root.front() != '@' && localAllocas.count(root) == 0))) {
          return std::nullopt;
        }
        continue;
      }
      if (instruction.opcode() != "call" ||
          instruction.operands().empty() ||
          instruction.operands().front() == "@" + function.name()) {
        continue;
      }
      const auto& symbol = instruction.operands().front();
      if (symbol.size() < 2 || symbol.front() != '@') {
        return std::nullopt;
      }
      const auto callee = functionIndices.find(symbol.substr(1));
      if (callee == functionIndices.end()) {
        return std::nullopt;
      }
      const auto& calleeEffect =
          effects.effectFor(module.functions()[callee->second].name());
      if (calleeEffect.writesExternalMemory ||
          calleeEffect.hasOtherSideEffects) {
        return std::nullopt;
      }
    }
  }
  return Candidate{
      functionIndex, 0, true,
      function.parameters().size() == 1 &&
          std::getenv("COMPILER_DISABLE_READONLY_MEMO_SECONDARY_PROBE") ==
              nullptr};
}

std::string freshSymbol(const Module &module, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &global : module.globals())
    names.insert(global.name());
  for (const auto &function : module.functions())
    names.insert(function.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = stem + std::to_string(suffix);
    if (names.find(candidate) == names.end())
      return candidate;
  }
}

std::string freshBlock(std::unordered_set<std::string> &reserved,
                       const std::string &stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = stem + std::to_string(suffix);
    if (reserved.insert(candidate).second)
      return candidate;
  }
}

std::string freshValue(const Function &function, const std::string &stem,
                       std::unordered_set<std::string> &reserved) {
  if (reserved.empty()) {
    for (const auto &parameter : function.parameters())
      reserved.insert(parameter.name);
    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        if (instruction.hasResult())
          reserved.insert(instruction.result());
      }
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const auto candidate = "%" + stem + "." + std::to_string(suffix);
    if (reserved.insert(candidate).second)
      return candidate;
  }
}

void addZeroArray(Module &module, const std::string &name,
                  std::size_t capacity) {
  const auto type = "int[" + std::to_string(capacity) + "]";
  module.addGlobal(GlobalVariable{name, type, Constant::aggregate(type, {})});
}

void applyMemoization(Module &module, const Candidate &candidate) {
  const auto functionName = module.functions()[candidate.function].name();
  const auto validName = freshSymbol(module, "__recursive_memo_valid_");
  addZeroArray(module, validName, candidate.capacity);
  std::vector<std::string> keyNames;
  keyNames.reserve(module.functions()[candidate.function].parameters().size());
  for (std::size_t parameter = 0;
       parameter < module.functions()[candidate.function].parameters().size();
       ++parameter) {
    const auto name = freshSymbol(module, "__recursive_memo_key_");
    addZeroArray(module, name, candidate.capacity);
    keyNames.push_back(name);
  }
  const auto valueName = freshSymbol(module, "__recursive_memo_value_");
  addZeroArray(module, valueName, candidate.capacity);

  auto &function = module.functions()[candidate.function];
  std::unordered_set<std::string> values;
  const auto hash = freshValue(function, "recursive.memo.hash", values);
  const auto index = freshValue(function, "recursive.memo.index", values);
  const auto secondaryUnmasked =
      candidate.twoProbeLoopCache
          ? freshValue(function, "recursive.memo.secondary.unmasked", values)
          : std::string{};
  const auto secondaryIndex =
      candidate.twoProbeLoopCache
          ? freshValue(function, "recursive.memo.secondary.index", values)
          : std::string{};
  const auto storeIndex =
      candidate.twoProbeLoopCache
          ? freshValue(function, "recursive.memo.store.index", values)
          : index;
  const auto validPointer =
      freshValue(function, "recursive.memo.valid.pointer", values);
  const auto valid = freshValue(function, "recursive.memo.valid", values);
  const auto validCondition =
      freshValue(function, "recursive.memo.valid.condition", values);
  const auto hitValuePointer =
      freshValue(function, "recursive.memo.hit.pointer", values);
  const auto hitValue =
      freshValue(function, "recursive.memo.hit.value", values);
  const auto originalEntry = function.blocks().front().name();
  std::unordered_set<std::string> blockNames;
  for (const auto &block : function.blocks())
    blockNames.insert(block.name());
  const auto entryName = freshBlock(blockNames, "recursive.memo.entry.");
  const auto hitName = freshBlock(blockNames, "recursive.memo.hit.");
  const auto secondaryValidName =
      candidate.twoProbeLoopCache
          ? freshBlock(blockNames, "recursive.memo.secondary.valid.")
          : std::string{};
  const auto secondaryCheckName =
      candidate.twoProbeLoopCache
          ? freshBlock(blockNames, "recursive.memo.secondary.check.")
          : std::string{};
  const auto secondaryHitName =
      candidate.twoProbeLoopCache
          ? freshBlock(blockNames, "recursive.memo.secondary.hit.")
          : std::string{};
  const auto missName =
      candidate.twoProbeLoopCache
          ? freshBlock(blockNames, "recursive.memo.miss.")
          : std::string{};
  std::vector<std::string> checkNames;
  for (std::size_t parameter = 0; parameter < function.parameters().size();
       ++parameter)
    checkNames.push_back(freshBlock(blockNames, "recursive.memo.check."));

  BasicBlock entry{entryName};
  std::string currentHash = function.parameters().front().name;
  for (std::size_t parameter = 1; parameter < function.parameters().size();
       ++parameter) {
    const auto multiplied =
        freshValue(function, "recursive.memo.hash.mul", values);
    const auto combined =
        parameter + 1 == function.parameters().size()
            ? hash
            : freshValue(function, "recursive.memo.hash.xor", values);
    entry.addInstruction(
        Instruction{"mul", {currentHash, "16777619"}, multiplied});
    entry.addInstruction(Instruction{
        "xor", {multiplied, function.parameters()[parameter].name}, combined});
    currentHash = combined;
  }
  if (function.parameters().size() == 1) {
    entry.addInstruction(Instruction{"add", {currentHash, "0"}, hash});
    currentHash = hash;
  }
  entry.addInstruction(Instruction{
      "and", {currentHash, std::to_string(candidate.capacity - 1)}, index});
  if (candidate.twoProbeLoopCache) {
    entry.addInstruction(
        Instruction{"add", {index, "1"}, secondaryUnmasked});
    entry.addInstruction(
        Instruction{"and",
                    {secondaryUnmasked,
                     std::to_string(candidate.capacity - 1)},
                    secondaryIndex});
  }
  entry.addInstruction(
      Instruction{"getelementptr", {"@" + validName, index}, validPointer});
  entry.addInstruction(Instruction{"load", {"int", validPointer}, valid});
  entry.addInstruction(Instruction{"icmp.ne", {valid, "0"}, validCondition});
  entry.addInstruction(
      Instruction{"br",
                  {validCondition, checkNames.front(),
                   candidate.twoProbeLoopCache ? missName : originalEntry}});

  std::vector<BasicBlock> checks;
  checks.reserve(function.parameters().size());
  for (std::size_t parameter = 0; parameter < function.parameters().size();
       ++parameter) {
    BasicBlock check{checkNames[parameter]};
    const auto pointer =
        freshValue(function, "recursive.memo.key.pointer", values);
    const auto key = freshValue(function, "recursive.memo.key", values);
    const auto equal = freshValue(function, "recursive.memo.key.equal", values);
    check.addInstruction(Instruction{
        "getelementptr", {"@" + keyNames[parameter], index}, pointer});
    check.addInstruction(Instruction{"load", {"int", pointer}, key});
    check.addInstruction(Instruction{
        "icmp.eq", {key, function.parameters()[parameter].name}, equal});
    check.addInstruction(
        Instruction{"br",
                    {equal,
                     parameter + 1 == function.parameters().size()
                         ? hitName
                         : checkNames[parameter + 1],
                     candidate.twoProbeLoopCache &&
                             parameter + 1 == function.parameters().size()
                         ? secondaryValidName
                         : originalEntry}});
    checks.push_back(std::move(check));
  }

  BasicBlock hit{hitName};
  hit.addInstruction(
      Instruction{"getelementptr", {"@" + valueName, index}, hitValuePointer});
  hit.addInstruction(Instruction{"load", {"int", hitValuePointer}, hitValue});
  hit.addInstruction(Instruction{"ret", {hitValue}});

  std::vector<BasicBlock> secondaryBlocks;
  if (candidate.twoProbeLoopCache) {
    const auto secondaryValidPointer =
        freshValue(function, "recursive.memo.secondary.valid.pointer", values);
    const auto secondaryValid =
        freshValue(function, "recursive.memo.secondary.valid.value", values);
    const auto secondaryValidCondition = freshValue(
        function, "recursive.memo.secondary.valid.condition", values);
    BasicBlock secondaryValidBlock{secondaryValidName};
    secondaryValidBlock.addInstruction(
        Instruction{"getelementptr",
                    {"@" + validName, secondaryIndex},
                    secondaryValidPointer});
    secondaryValidBlock.addInstruction(
        Instruction{"load", {"int", secondaryValidPointer}, secondaryValid});
    secondaryValidBlock.addInstruction(
        Instruction{"icmp.ne", {secondaryValid, "0"},
                    secondaryValidCondition});
    secondaryValidBlock.addInstruction(
        Instruction{"br",
                    {secondaryValidCondition, secondaryCheckName, missName}});
    secondaryBlocks.push_back(std::move(secondaryValidBlock));

    const auto secondaryKeyPointer =
        freshValue(function, "recursive.memo.secondary.key.pointer", values);
    const auto secondaryKey =
        freshValue(function, "recursive.memo.secondary.key", values);
    const auto secondaryEqual =
        freshValue(function, "recursive.memo.secondary.key.equal", values);
    BasicBlock secondaryCheck{secondaryCheckName};
    secondaryCheck.addInstruction(
        Instruction{"getelementptr",
                    {"@" + keyNames.front(), secondaryIndex},
                    secondaryKeyPointer});
    secondaryCheck.addInstruction(
        Instruction{"load", {"int", secondaryKeyPointer}, secondaryKey});
    secondaryCheck.addInstruction(
        Instruction{"icmp.eq",
                    {secondaryKey, function.parameters().front().name},
                    secondaryEqual});
    secondaryCheck.addInstruction(
        Instruction{"br", {secondaryEqual, secondaryHitName, missName}});
    secondaryBlocks.push_back(std::move(secondaryCheck));

    const auto secondaryHitPointer =
        freshValue(function, "recursive.memo.secondary.hit.pointer", values);
    const auto secondaryHitValue =
        freshValue(function, "recursive.memo.secondary.hit.value", values);
    BasicBlock secondaryHit{secondaryHitName};
    secondaryHit.addInstruction(
        Instruction{"getelementptr",
                    {"@" + valueName, secondaryIndex},
                    secondaryHitPointer});
    secondaryHit.addInstruction(
        Instruction{"load", {"int", secondaryHitPointer}, secondaryHitValue});
    secondaryHit.addInstruction(Instruction{"ret", {secondaryHitValue}});
    secondaryBlocks.push_back(std::move(secondaryHit));

    BasicBlock miss{missName};
    miss.addInstruction(
        Instruction{"phi",
                    {"int", index, entryName, secondaryIndex,
                     secondaryValidName, index, secondaryCheckName},
                    storeIndex});
    miss.addInstruction(Instruction{"br", {originalEntry}});
    secondaryBlocks.push_back(std::move(miss));
  }

  std::vector<BasicBlock> stores;
  for (auto &block : function.blocks()) {
    auto &instructions = block.instructions();
    if (instructions.empty() || instructions.back().opcode() != "ret" ||
        instructions.back().operands().size() != 1)
      continue;
    const auto result = instructions.back().operands()[0];
    const auto storeName = freshBlock(blockNames, "recursive.memo.store.");
    instructions.back() = Instruction{"br", {storeName}};
    BasicBlock store{storeName};
    for (std::size_t parameter = 0; parameter < function.parameters().size();
         ++parameter) {
      const auto pointer =
          freshValue(function, "recursive.memo.store.key.pointer", values);
      store.addInstruction(Instruction{
          "getelementptr", {"@" + keyNames[parameter], storeIndex}, pointer});
      store.addInstruction(Instruction{
          "store", {"int", function.parameters()[parameter].name, pointer}});
    }
    const auto resultPointer =
        freshValue(function, "recursive.memo.store.value.pointer", values);
    const auto storeValidPointer =
        freshValue(function, "recursive.memo.store.valid.pointer", values);
    store.addInstruction(
        Instruction{"getelementptr",
                    {"@" + valueName, storeIndex},
                    resultPointer});
    store.addInstruction(Instruction{"store", {"int", result, resultPointer}});
    store.addInstruction(Instruction{
        "getelementptr", {"@" + validName, storeIndex}, storeValidPointer});
    store.addInstruction(Instruction{"store", {"int", "1", storeValidPointer}});
    store.addInstruction(Instruction{"ret", {result}});
    stores.push_back(std::move(store));
  }

  function.blocks().push_front(std::move(hit));
  for (auto iterator = secondaryBlocks.rbegin();
       iterator != secondaryBlocks.rend(); ++iterator)
    function.blocks().push_front(std::move(*iterator));
  for (auto iterator = checks.rbegin(); iterator != checks.rend(); ++iterator)
    function.blocks().push_front(std::move(*iterator));
  function.blocks().push_front(std::move(entry));
  for (auto &store : stores)
    function.addBlock(std::move(store));
  if (candidate.readOnlyLoopLifetime) {
    module.addPassNote(
        candidate.twoProbeLoopCache
            ? "memoized a loop-contained single-key read-only helper with a "
              "conflict-only secondary probe after proving its global read "
              "set immutable for the cache lifetime"
            : "memoized a loop-contained read-only helper after proving its "
              "global read set immutable for the cache lifetime");
  } else {
    module.addPassNote("memoized a proven read-only branching recursion using "
                       "full integer parameter keys");
  }
  (void)functionName;
}

bool markSingleImmutableTailMemoContext(Module& module) {
  const GlobalVariable* context = nullptr;
  for (const auto& global : module.globals()) {
    if (global.isConst()) {
      continue;
    }
    if (context != nullptr || global.type() != "int") {
      return false;
    }
    context = &global;
  }
  if (context == nullptr) {
    return false;
  }

  const std::string symbol = "@" + context->name();
  std::optional<CallSite> uniqueStore;
  std::unordered_set<std::size_t> readers;
  for (std::size_t functionIndex = 0;
       functionIndex < module.functions().size(); ++functionIndex) {
    const auto& function = module.functions()[functionIndex];
    for (const auto& parameter : function.parameters()) {
      if (parameter.type != "int" && parameter.type != "float") {
        return false;
      }
    }
    for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
         ++blockIndex) {
      const auto& instructions = function.blocks()[blockIndex].instructions();
      for (std::size_t instructionIndex = 0;
           instructionIndex < instructions.size(); ++instructionIndex) {
        const auto& instruction = instructions[instructionIndex];
        const auto& operands = instruction.operands();
        const bool mentionsContext =
            std::find(operands.begin(), operands.end(), symbol) !=
            operands.end();
        if (instruction.opcode() == "load" && !operands.empty() &&
            operands.back() == symbol) {
          readers.insert(functionIndex);
          continue;
        }
        if (instruction.opcode() == "store") {
          if (operands.empty()) {
            return false;
          }
          const auto& destination = operands.back();
          if (destination == symbol) {
            if (uniqueStore.has_value()) {
              return false;
            }
            uniqueStore =
                CallSite{functionIndex, blockIndex, instructionIndex};
            continue;
          }
          // At this early optimized-IR point, reject any computed store:
          // without object-sensitive alias proof it could target the context.
          if (destination.empty() || destination.front() != '@') {
            return false;
          }
        }
        if (mentionsContext) {
          return false;
        }
      }
    }
  }
  if (!uniqueStore.has_value() || readers.empty()) {
    return false;
  }

  const auto& storeFunction = module.functions()[uniqueStore->caller];
  if (storeFunction.name() != "main" || uniqueStore->block != 0 ||
      storeFunction.blocks().empty()) {
    return false;
  }
  for (const auto readerIndex : readers) {
    if (readerIndex == uniqueStore->caller) {
      const auto& entryInstructions =
          storeFunction.blocks().front().instructions();
      for (std::size_t index = 0; index < uniqueStore->instruction; ++index) {
        const auto& instruction = entryInstructions[index];
        if (instruction.opcode() == "load" &&
            !instruction.operands().empty() &&
            instruction.operands().back() == symbol) {
          return false;
        }
      }
      continue;
    }
    const auto calls =
        callsTo(module, "@" + module.functions()[readerIndex].name());
    if (calls.empty()) {
      return false;
    }
    for (const auto& call : calls) {
      if (call.caller != uniqueStore->caller) {
        return false;
      }
      if (call.block == 0 && call.instruction < uniqueStore->instruction) {
        return false;
      }
    }
  }

  module.markSingleImmutableTailMemoContext();
  module.addPassNote(
      "proved a single immutable scalar context for compact tail memoization");
  return true;
}

} // namespace

void RecursiveMemoizationPass::run(Module &module) {
  const bool markedSingleContext =
      std::getenv("COMPILER_DISABLE_COMPACT_TAIL_MEMOIZATION") == nullptr &&
      markSingleImmutableTailMemoContext(module);
  FunctionEffectAnalysis effects(module);
  std::vector<Candidate> candidates;
  candidates.reserve(module.functions().size());
  std::size_t readOnlyLoopCandidates = 0;
  for (std::size_t function = 0; function < module.functions().size();
       ++function) {
    const auto candidate = findCandidate(module, function, effects);
    if (candidate.has_value()) {
      candidates.push_back(*candidate);
      continue;
    }
    const auto loopCandidate =
        markedSingleContext
            ? std::optional<Candidate>{}
            : findReadOnlyLoopCandidate(module, function, effects);
    if (loopCandidate.has_value()) {
      candidates.push_back(*loopCandidate);
      ++readOnlyLoopCandidates;
    }
  }

  std::size_t memoized = 0;
  const auto perCandidateBudget =
      candidates.empty() ? 0 : kModuleCacheBudgetBytes / candidates.size();
  for (auto &candidate : candidates) {
    const auto candidateBudget = candidate.readOnlyLoopLifetime
                                     ? kReadOnlyLoopCacheBudgetBytes
                                     : perCandidateBudget;
    candidate.capacity = cacheCapacity(
        module.functions()[candidate.function].parameters().size(),
        candidateBudget);
    if (candidate.capacity == 0)
      continue;
    applyMemoization(module, candidate);
    ++memoized;
  }
  if (std::getenv("COMPILER_RECURSIVE_MEMOIZATION_STATS") != nullptr)
    std::cerr << "recursive-memoization: memoized=" << memoized
              << " single-context=" << (markedSingleContext ? 1 : 0)
              << " loop-readonly=" << readOnlyLoopCandidates << '\n';
}

} // namespace compiler::midend::pass
