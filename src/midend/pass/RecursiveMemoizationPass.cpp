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
constexpr std::size_t kMinimumCacheEntries = 1024U;

struct CallSite {
  std::size_t caller = 0;
  std::size_t block = 0;
  std::size_t instruction = 0;
};

struct Candidate {
  std::size_t function = 0;
  std::size_t capacity = 0;
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
  entry.addInstruction(
      Instruction{"getelementptr", {"@" + validName, index}, validPointer});
  entry.addInstruction(Instruction{"load", {"int", validPointer}, valid});
  entry.addInstruction(Instruction{"icmp.ne", {valid, "0"}, validCondition});
  entry.addInstruction(
      Instruction{"br", {validCondition, checkNames.front(), originalEntry}});

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
                     originalEntry}});
    checks.push_back(std::move(check));
  }

  BasicBlock hit{hitName};
  hit.addInstruction(
      Instruction{"getelementptr", {"@" + valueName, index}, hitValuePointer});
  hit.addInstruction(Instruction{"load", {"int", hitValuePointer}, hitValue});
  hit.addInstruction(Instruction{"ret", {hitValue}});

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
          "getelementptr", {"@" + keyNames[parameter], index}, pointer});
      store.addInstruction(Instruction{
          "store", {"int", function.parameters()[parameter].name, pointer}});
    }
    const auto resultPointer =
        freshValue(function, "recursive.memo.store.value.pointer", values);
    const auto storeValidPointer =
        freshValue(function, "recursive.memo.store.valid.pointer", values);
    store.addInstruction(
        Instruction{"getelementptr", {"@" + valueName, index}, resultPointer});
    store.addInstruction(Instruction{"store", {"int", result, resultPointer}});
    store.addInstruction(Instruction{
        "getelementptr", {"@" + validName, index}, storeValidPointer});
    store.addInstruction(Instruction{"store", {"int", "1", storeValidPointer}});
    store.addInstruction(Instruction{"ret", {result}});
    stores.push_back(std::move(store));
  }

  function.blocks().push_front(std::move(hit));
  for (auto iterator = checks.rbegin(); iterator != checks.rend(); ++iterator)
    function.blocks().push_front(std::move(*iterator));
  function.blocks().push_front(std::move(entry));
  for (auto &store : stores)
    function.addBlock(std::move(store));
  module.addPassNote("memoized a proven read-only branching recursion using "
                     "full integer parameter keys");
  (void)functionName;
}

} // namespace

void RecursiveMemoizationPass::run(Module &module) {
  FunctionEffectAnalysis effects(module);
  std::vector<Candidate> candidates;
  candidates.reserve(module.functions().size());
  for (std::size_t function = 0; function < module.functions().size();
       ++function) {
    const auto candidate = findCandidate(module, function, effects);
    if (candidate.has_value())
      candidates.push_back(*candidate);
  }

  std::size_t memoized = 0;
  const auto perCandidateBudget =
      candidates.empty() ? 0 : kModuleCacheBudgetBytes / candidates.size();
  for (auto &candidate : candidates) {
    candidate.capacity = cacheCapacity(
        module.functions()[candidate.function].parameters().size(),
        perCandidateBudget);
    if (candidate.capacity == 0)
      continue;
    applyMemoization(module, candidate);
    ++memoized;
  }
  if (std::getenv("COMPILER_RECURSIVE_MEMOIZATION_STATS") != nullptr)
    std::cerr << "recursive-memoization: memoized=" << memoized << '\n';
}

} // namespace compiler::midend::pass
