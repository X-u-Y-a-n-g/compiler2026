#include "../../../include/compiler/midend/pass/LoopInvariantCodeMotionPass.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;
using compiler::midend::analysis::FunctionEffectAnalysis;

struct BlockInfo {
  std::string name;
  std::vector<std::size_t> preds;
  std::vector<std::size_t> succs;
};

struct LoopInfo {
  std::size_t header = 0;
  std::size_t preheader = 0;
  std::unordered_set<std::size_t> blocks;
};

struct CalleeSummary {
  std::string returnType;
  std::size_t instructionCount = 0;
  bool recursive = false;
};

bool isValueReference(const std::string& operand) {
  return !operand.empty() && operand.front() == '%';
}

std::string calledFunctionName(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& callee = instruction.operands().front();
  if (callee.size() < 2 || callee.front() != '@') {
    return {};
  }
  return callee.substr(1);
}

std::unordered_map<std::string, CalleeSummary> summarizeCallees(const Module& module) {
  std::unordered_map<std::string, std::vector<std::string>> callGraph;
  std::unordered_map<std::string, CalleeSummary> summaries;
  for (const auto& function : module.functions()) {
    auto& summary = summaries[function.name()];
    summary.returnType = function.returnType();
    auto& callees = callGraph[function.name()];
    for (const auto& block : function.blocks()) {
      summary.instructionCount += block.instructions().size();
      for (const auto& instruction : block.instructions()) {
        const auto callee = calledFunctionName(instruction);
        if (!callee.empty()) {
          callees.push_back(callee);
        }
      }
    }
  }

  for (auto& [functionName, summary] : summaries) {
    std::deque<std::string> worklist;
    std::unordered_set<std::string> visited;
    for (const auto& callee : callGraph[functionName]) {
      worklist.push_back(callee);
    }
    while (!worklist.empty()) {
      auto current = std::move(worklist.front());
      worklist.pop_front();
      if (current == functionName) {
        summary.recursive = true;
        break;
      }
      if (!visited.insert(current).second) {
        continue;
      }
      const auto nested = callGraph.find(current);
      if (nested == callGraph.end()) {
        continue;
      }
      for (const auto& callee : nested->second) {
        worklist.push_back(callee);
      }
    }
  }
  return summaries;
}

std::vector<BlockInfo> buildCFG(const Function& function) {
  std::vector<BlockInfo> cfg;
  cfg.reserve(function.blocks().size());
  std::unordered_map<std::string, std::size_t> indexByName;

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    cfg.push_back(BlockInfo{function.blocks()[i].name(), {}, {}});
    indexByName.emplace(function.blocks()[i].name(), i);
  }

  for (std::size_t i = 0; i < function.blocks().size(); ++i) {
    const auto* terminator = function.blocks()[i].terminator();
    if (terminator == nullptr || terminator->opcode() != "br") {
      continue;
    }

    const auto& operands = terminator->operands();
    if (operands.size() == 1) {
      const auto target = indexByName.find(operands[0]);
      if (target != indexByName.end()) {
        cfg[i].succs.push_back(target->second);
      }
    } else if (operands.size() == 3) {
      const auto trueTarget = indexByName.find(operands[1]);
      const auto falseTarget = indexByName.find(operands[2]);
      if (trueTarget != indexByName.end()) {
        cfg[i].succs.push_back(trueTarget->second);
      }
      if (falseTarget != indexByName.end() && falseTarget != trueTarget) {
        cfg[i].succs.push_back(falseTarget->second);
      }
    }
  }

  for (std::size_t i = 0; i < cfg.size(); ++i) {
    for (const auto succ : cfg[i].succs) {
      cfg[succ].preds.push_back(i);
    }
  }

  return cfg;
}

std::vector<bool> computeReachable(const std::vector<BlockInfo>& cfg) {
  std::vector<bool> reachable(cfg.size(), false);
  if (cfg.empty()) {
    return reachable;
  }

  std::deque<std::size_t> worklist;
  reachable[0] = true;
  worklist.push_back(0);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto succ : cfg[block].succs) {
      if (!reachable[succ]) {
        reachable[succ] = true;
        worklist.push_back(succ);
      }
    }
  }
  return reachable;
}

std::vector<std::size_t> computeReversePostOrder(const std::vector<BlockInfo>& cfg,
                                                 const std::vector<bool>& reachable) {
  std::vector<std::size_t> order;
  if (cfg.empty()) {
    return order;
  }

  std::vector<bool> visited(cfg.size(), false);
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  visited[0] = true;
  stack.emplace_back(0, 0);
  while (!stack.empty()) {
    auto& [block, nextSucc] = stack.back();
    if (nextSucc < cfg[block].succs.size()) {
      const auto succ = cfg[block].succs[nextSucc++];
      if (reachable[succ] && !visited[succ]) {
        visited[succ] = true;
        stack.emplace_back(succ, 0);
      }
      continue;
    }
    order.push_back(block);
    stack.pop_back();
  }

  std::reverse(order.begin(), order.end());
  return order;
}

std::size_t intersectIdoms(std::size_t lhs, std::size_t rhs, const std::vector<int>& idom,
                           const std::vector<std::size_t>& rpoIndex) {
  while (lhs != rhs) {
    while (rpoIndex[lhs] > rpoIndex[rhs]) {
      lhs = static_cast<std::size_t>(idom[lhs]);
    }
    while (rpoIndex[rhs] > rpoIndex[lhs]) {
      rhs = static_cast<std::size_t>(idom[rhs]);
    }
  }
  return lhs;
}

std::vector<int> computeImmediateDominators(const std::vector<BlockInfo>& cfg,
                                            const std::vector<bool>& reachable) {
  const std::size_t n = cfg.size();
  std::vector<int> idom(n, -1);
  if (n == 0) {
    return idom;
  }

  const auto rpo = computeReversePostOrder(cfg, reachable);
  std::vector<std::size_t> rpoIndex(n, n);
  for (std::size_t i = 0; i < rpo.size(); ++i) {
    rpoIndex[rpo[i]] = i;
  }

  idom[0] = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto block : rpo) {
      if (block == 0) {
        continue;
      }

      std::optional<std::size_t> next;
      for (const auto pred : cfg[block].preds) {
        if (!reachable[pred] || idom[pred] < 0) {
          continue;
        }
        if (!next.has_value()) {
          next = pred;
        } else {
          next = intersectIdoms(pred, *next, idom, rpoIndex);
        }
      }

      if (next.has_value() && idom[block] != static_cast<int>(*next)) {
        idom[block] = static_cast<int>(*next);
        changed = true;
      }
    }
  }

  return idom;
}

bool dominates(std::size_t dominator, std::size_t block, const std::vector<int>& idom) {
  if (dominator == block) {
    return true;
  }
  while (block < idom.size() && idom[block] >= 0 && static_cast<std::size_t>(idom[block]) != block) {
    block = static_cast<std::size_t>(idom[block]);
    if (block == dominator) {
      return true;
    }
  }
  return false;
}

std::optional<LoopInfo> buildNaturalLoop(const std::vector<BlockInfo>& cfg, std::size_t header,
                                         std::size_t latch) {
  LoopInfo loop;
  loop.header = header;
  loop.blocks.insert(header);
  loop.blocks.insert(latch);

  std::deque<std::size_t> worklist;
  worklist.push_back(latch);
  while (!worklist.empty()) {
    const auto block = worklist.front();
    worklist.pop_front();
    for (const auto pred : cfg[block].preds) {
      if (loop.blocks.insert(pred).second) {
        worklist.push_back(pred);
      }
    }
  }

  std::optional<std::size_t> preheader;
  for (const auto pred : cfg[header].preds) {
    if (loop.blocks.find(pred) != loop.blocks.end()) {
      continue;
    }
    if (preheader.has_value()) {
      return std::nullopt;
    }
    preheader = pred;
  }
  if (!preheader.has_value()) {
    return std::nullopt;
  }

  loop.preheader = *preheader;
  return loop;
}

std::vector<LoopInfo> findLoops(const std::vector<BlockInfo>& cfg, const std::vector<int>& idom,
                                const std::vector<bool>& reachable) {
  std::vector<LoopInfo> loops;
  for (std::size_t latch = 0; latch < cfg.size(); ++latch) {
    if (!reachable[latch]) {
      continue;
    }
    for (const auto header : cfg[latch].succs) {
      if (!reachable[header] || !dominates(header, latch, idom)) {
        continue;
      }
      auto loop = buildNaturalLoop(cfg, header, latch);
      if (loop.has_value()) {
        loops.push_back(std::move(*loop));
      }
    }
  }

  std::sort(loops.begin(), loops.end(), [](const LoopInfo& lhs, const LoopInfo& rhs) {
    return lhs.blocks.size() < rhs.blocks.size();
  });
  return loops;
}

std::unordered_map<std::string, std::size_t> computeDefinitionBlocks(const Function& function) {
  std::unordered_map<std::string, std::size_t> definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult()) {
        definitions[instruction.result()] = block;
      }
    }
  }
  return definitions;
}

bool isHoistableCandidate(const Instruction& instruction) {
  if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
    return false;
  }
  const auto& opcode = instruction.opcode();
  const bool isGep = opcode == "gep" || opcode == "gep.linear" ||
                     opcode == "getelementptr" || opcode == "getelementptr.linear";
  return isGep && !instruction.operands().empty() &&
         !instruction.operands().front().empty() &&
         instruction.operands().front().front() == '@';
}

bool operandsAreLoopInvariant(
    const Instruction& instruction, const LoopInfo& loop,
    const std::unordered_map<std::string, std::size_t>& definitions,
    const std::unordered_set<std::string>& selected) {
  for (const auto& operand : instruction.operands()) {
    if (!isValueReference(operand)) {
      continue;
    }
    const auto definition = definitions.find(operand);
    if (definition == definitions.end()) {
      continue;
    }
    if (loop.blocks.find(definition->second) == loop.blocks.end()) {
      continue;
    }
    if (selected.find(operand) != selected.end()) {
      continue;
    }
    return false;
  }
  return true;
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
                      const std::string& prefix, const std::string& separator) {
  std::string candidate = prefix;
  std::size_t suffix = 0;
  while (!names.insert(candidate).second) {
    candidate = prefix + separator + std::to_string(++suffix);
  }
  return candidate;
}

void rewritePhiPredecessor(Function& function, const std::string& oldPredecessor,
                           const std::string& newPredecessor) {
  for (auto& block : function.blocks()) {
    for (auto& instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        continue;
      }
      auto operands = instruction.operands();
      bool changed = false;
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == oldPredecessor) {
          operands[index] = newPredecessor;
          changed = true;
        }
      }
      if (changed) {
        instruction = Instruction{"phi", std::move(operands), instruction.result()};
      }
    }
  }
}

bool isProfitableInvariantCall(
    const Instruction& instruction,
    const FunctionEffectAnalysis& functionEffects,
    const std::unordered_map<std::string, CalleeSummary>& summaries,
    std::string& returnType) {
  if (instruction.opcode() != "call" || !instruction.hasResult()) {
    return false;
  }
  const auto callee = calledFunctionName(instruction);
  if (callee.empty() || !functionEffects.isReadNone(callee)) {
    return false;
  }
  const auto summary = summaries.find(callee);
  if (summary == summaries.end() || summary->second.returnType == "void") {
    return false;
  }

  constexpr std::size_t kMinimumInstructionCount = 24;
  if (!summary->second.recursive &&
      summary->second.instructionCount < kMinimumInstructionCount) {
    return false;
  }
  returnType = summary->second.returnType;
  return true;
}

struct ValueDefinition {
  std::size_t block = 0;
  Instruction instruction{""};
};

bool isSpeculatableInvariantDependency(const Instruction& instruction) {
  if (!instruction.hasResult() || !detail::isSideEffectFree(instruction)) {
    return false;
  }
  const auto& opcode = instruction.opcode();
  return opcode != "alloca" && opcode != "load" && opcode != "phi" &&
         opcode != "sdiv" && opcode != "udiv" && opcode != "srem" &&
         opcode != "urem";
}

bool materializeInvariantValue(
    const std::string& value, const LoopInfo& loop,
    const std::unordered_map<std::string, ValueDefinition>& definitions,
    std::unordered_set<std::string>& visiting,
    std::unordered_map<std::string, std::string>& replacements,
    std::vector<Instruction>& materializations,
    std::unordered_set<std::string>& valueNames) {
  if (!isValueReference(value)) {
    return true;
  }
  const auto definition = definitions.find(value);
  if (definition == definitions.end() ||
      loop.blocks.find(definition->second.block) == loop.blocks.end()) {
    return true;
  }
  if (replacements.find(value) != replacements.end()) {
    return true;
  }
  if (!isSpeculatableInvariantDependency(definition->second.instruction) ||
      !visiting.insert(value).second) {
    return false;
  }

  auto operands = definition->second.instruction.operands();
  for (auto& operand : operands) {
    if (!materializeInvariantValue(operand, loop, definitions, visiting,
                                   replacements, materializations, valueNames)) {
      visiting.erase(value);
      return false;
    }
    if (const auto replacement = replacements.find(operand);
        replacement != replacements.end()) {
      operand = replacement->second;
    }
  }

  const auto clonedResult =
      freshName(valueNames, "%licm.call.cache.invariant", ".");
  materializations.emplace_back(definition->second.instruction.opcode(),
                                std::move(operands), clonedResult);
  replacements.emplace(value, clonedResult);
  visiting.erase(value);
  return true;
}

bool collectInvariantMaterializations(
    const Instruction& call, const LoopInfo& loop,
    const std::unordered_map<std::string, ValueDefinition>& definitions,
    std::unordered_map<std::string, std::string>& replacements,
    std::vector<Instruction>& materializations,
    std::unordered_set<std::string>& valueNames) {
  std::unordered_set<std::string> visiting;
  for (std::size_t index = 1; index < call.operands().size(); ++index) {
    if (!materializeInvariantValue(call.operands()[index], loop, definitions,
                                   visiting, replacements, materializations,
                                   valueNames)) {
      return false;
    }
  }
  return true;
}

bool cacheOneInvariantCall(
    Function& function, const FunctionEffectAnalysis& functionEffects,
    const std::unordered_map<std::string, CalleeSummary>& summaries) {
  const auto cfg = buildCFG(function);
  if (cfg.empty()) {
    return false;
  }
  const auto reachable = computeReachable(cfg);
  const auto idom = computeImmediateDominators(cfg, reachable);
  const auto loops = findLoops(cfg, idom, reachable);
  std::unordered_map<std::string, ValueDefinition> definitions;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto& instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult()) {
        definitions.emplace(instruction.result(),
                            ValueDefinition{block, instruction});
      }
    }
  }

  for (const auto& loop : loops) {
    for (const auto blockIndex : loop.blocks) {
      if (blockIndex == loop.header ||
          function.blocks()[blockIndex].name().find("licm.call.cache.") == 0) {
        continue;
      }
      const auto& instructions = function.blocks()[blockIndex].instructions();
      for (std::size_t instructionIndex = 0;
           instructionIndex < instructions.size(); ++instructionIndex) {
        const auto& instruction = instructions[instructionIndex];
        std::string returnType;
        if (!isProfitableInvariantCall(instruction, functionEffects, summaries,
                                       returnType)) {
          continue;
        }

        auto valueNames = collectValueNames(function);
        std::unordered_map<std::string, std::string> invariantReplacements;
        std::vector<Instruction> invariantMaterializations;
        if (!collectInvariantMaterializations(
                instruction, loop, definitions, invariantReplacements,
                invariantMaterializations, valueNames)) {
          continue;
        }
        auto blockNames = collectBlockNames(function);
        const auto flagSlot = freshName(valueNames, "%licm.call.cache.flag", ".");
        const auto valueSlot = freshName(valueNames, "%licm.call.cache.value", ".");
        const auto loadedFlag = freshName(valueNames, "%licm.call.cache.loaded", ".");
        const auto hasValue = freshName(valueNames, "%licm.call.cache.valid", ".");
        const auto computedValue =
            freshName(valueNames, "%licm.call.cache.computed", ".");
        const auto checkName =
            freshName(blockNames, "licm.call.cache.check", ".");
        const auto computeName =
            freshName(blockNames, "licm.call.cache.compute", ".");
        const auto reuseName =
            freshName(blockNames, "licm.call.cache.reuse", ".");
        const auto continueName =
            freshName(blockNames, "licm.call.cache.continue", ".");

        auto& entryInstructions = function.blocks().front().instructions();
        entryInstructions.insert(
            entryInstructions.begin(),
            {Instruction{"alloca", {"int"}, flagSlot},
             Instruction{"alloca", {returnType}, valueSlot}});

        auto& preheaderInstructions =
            function.blocks()[loop.preheader].instructions();
        auto preheaderInsert = preheaderInstructions.end();
        if (!preheaderInstructions.empty() &&
            preheaderInstructions.back().isTerminator()) {
          preheaderInsert = preheaderInstructions.end() - 1;
        }
        preheaderInstructions.insert(preheaderInsert,
                                     Instruction{"store", {"int", "0", flagSlot}});

        const std::string originalBlockName = function.blocks()[blockIndex].name();
        const std::string originalResult = instruction.result();
        auto cachedCallOperands = instruction.operands();
        for (auto& operand : cachedCallOperands) {
          if (const auto replacement = invariantReplacements.find(operand);
              replacement != invariantReplacements.end()) {
            operand = replacement->second;
          }
        }
        Instruction cachedCall{instruction.opcode(), std::move(cachedCallOperands),
                               computedValue};
        std::vector<Instruction> prefix(instructions.begin(),
                                        instructions.begin() + instructionIndex);
        std::vector<Instruction> suffix(instructions.begin() + instructionIndex + 1,
                                        instructions.end());
        prefix.emplace_back("br", std::vector<std::string>{checkName});
        function.blocks()[blockIndex].instructions() = std::move(prefix);

        compiler::ir::core::BasicBlock checkBlock{checkName};
        checkBlock.addInstruction(Instruction{"load", {"int", flagSlot}, loadedFlag});
        checkBlock.addInstruction(Instruction{"icmp.ne", {loadedFlag, "0"}, hasValue});
        checkBlock.addInstruction(
            Instruction{"br", {hasValue, reuseName, computeName}});

        compiler::ir::core::BasicBlock computeBlock{computeName};
        for (auto& materialization : invariantMaterializations) {
          computeBlock.addInstruction(std::move(materialization));
        }
        computeBlock.addInstruction(std::move(cachedCall));
        computeBlock.addInstruction(
            Instruction{"store", {returnType, computedValue, valueSlot}});
        computeBlock.addInstruction(Instruction{"store", {"int", "1", flagSlot}});
        computeBlock.addInstruction(Instruction{"br", {continueName}});

        compiler::ir::core::BasicBlock reuseBlock{reuseName};
        reuseBlock.addInstruction(Instruction{"br", {continueName}});

        compiler::ir::core::BasicBlock continueBlock{continueName};
        continueBlock.addInstruction(
            Instruction{"load", {returnType, valueSlot}, originalResult});
        for (auto& remaining : suffix) {
          continueBlock.addInstruction(std::move(remaining));
        }

        rewritePhiPredecessor(function, originalBlockName, continueName);
        function.addBlock(std::move(checkBlock));
        function.addBlock(std::move(computeBlock));
        function.addBlock(std::move(reuseBlock));
        function.addBlock(std::move(continueBlock));
        return true;
      }
    }
  }
  return false;
}

bool hoistFromLoop(Function& function, const LoopInfo& loop,
                   const std::unordered_map<std::string, std::size_t>& definitions) {
  std::vector<std::pair<std::size_t, Instruction>> hoisted;
  std::unordered_set<std::string> selected;

  bool found = true;
  while (found) {
    found = false;
    for (const auto block : loop.blocks) {
      for (const auto& instruction : function.blocks()[block].instructions()) {
        if (!isHoistableCandidate(instruction) ||
            selected.find(instruction.result()) != selected.end()) {
          continue;
        }
        if (!operandsAreLoopInvariant(instruction, loop, definitions, selected)) {
          continue;
        }
        selected.insert(instruction.result());
        hoisted.emplace_back(block, instruction);
        found = true;
      }
    }
  }

  if (hoisted.empty()) {
    return false;
  }

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult() && selected.find(instruction.result()) != selected.end()) {
        continue;
      }
      rewritten.push_back(instruction);
    }
    block.instructions() = std::move(rewritten);
  }

  auto& preheaderInstructions = function.blocks()[loop.preheader].instructions();
  auto insertPos = preheaderInstructions.end();
  if (!preheaderInstructions.empty() && preheaderInstructions.back().isTerminator()) {
    insertPos = preheaderInstructions.end() - 1;
  }

  std::vector<Instruction> toInsert;
  toInsert.reserve(hoisted.size());
  for (auto& [unusedBlock, instruction] : hoisted) {
    (void)unusedBlock;
    toInsert.push_back(std::move(instruction));
  }
  preheaderInstructions.insert(insertPos, toInsert.begin(), toInsert.end());
  return true;
}

bool runLicmOnce(Function& function) {
  const auto cfg = buildCFG(function);
  if (cfg.empty()) {
    return false;
  }

  const auto reachable = computeReachable(cfg);
  const auto idom = computeImmediateDominators(cfg, reachable);
  const auto loops = findLoops(cfg, idom, reachable);
  if (loops.empty()) {
    return false;
  }

  bool changed = false;
  const auto definitions = computeDefinitionBlocks(function);
  for (const auto& loop : loops) {
    changed |= hoistFromLoop(function, loop, definitions);
  }
  return changed;
}

void runLicm(Function& function) {
  bool changed = false;
  do {
    changed = runLicmOnce(function);
  } while (changed);
}

}  // namespace

void LoopInvariantCodeMotionPass::run(compiler::ir::core::Module& module) {
  const FunctionEffectAnalysis functionEffects(module);
  const auto calleeSummaries = summarizeCallees(module);
  for (auto& function : module.functions()) {
    runLicm(function);
    while (cacheOneInvariantCall(function, functionEffects, calleeSummaries)) {
    }
  }
}

}  // namespace compiler::midend::pass
