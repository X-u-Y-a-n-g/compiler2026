#include "../../../include/compiler/midend/pass/AdjacentProducerConsumerLoopFusionPass.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
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

struct Definition {
  const Instruction* instruction = nullptr;
  std::string block;
};

using DefinitionMap = std::unordered_map<std::string, Definition>;

struct PointerPath {
  std::string base;
  std::vector<std::string> indices;
};

struct ScalarLoop {
  std::string header;
  std::string body;
  std::string exit;
  std::string counterSlot;
  std::string headerIndex;
  std::string bodyIndex;
  std::string bound;
};

struct LoopNest {
  std::string header;
  std::string body;
  std::string exit;
  std::string latch;
  std::string induction;
  std::string bound;
  ScalarLoop inner;
};

struct FusionCandidate {
  LoopNest producer;
  LoopNest consumer;
  std::string entryRedirectBlock;
  std::string producerStoreAddress;
  std::string producerStoredValue;
  std::string consumerLoad;
  std::string consumerStoredValue;
  std::string consumerStoreType;
  std::unordered_map<std::string, std::string> consumerAliases;
};

std::vector<std::string> branchTargets(const BasicBlock& block) {
  const auto* terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br") return {};
  if (terminator->operands().size() == 1) {
    return {terminator->operands()[0]};
  }
  if (terminator->operands().size() == 3) {
    if (terminator->operands()[1] == terminator->operands()[2]) {
      return {terminator->operands()[1]};
    }
    return {terminator->operands()[1], terminator->operands()[2]};
  }
  return {};
}

CFG buildCFG(const Function& function) {
  CFG cfg;
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& target : branchTargets(block)) {
      const auto found = cfg.index.find(target);
      if (found != cfg.index.end()) {
        cfg.predecessors[found->second].push_back(block.name());
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
        definitions.emplace(instruction.result(),
                            Definition{&instruction, block.name()});
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
    if (block.name() == name) return &block;
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

bool isZero(const std::string& value) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == 0;
}

bool isOne(const std::string& value) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == 1;
}

bool isUnitIncrement(const std::string& value, const std::string& induction,
                     const DefinitionMap& definitions,
                     const std::string& requiredBlock) {
  const auto found = definitions.find(value);
  if (found == definitions.end() || found->second.block != requiredBlock ||
      found->second.instruction->opcode() != "add" ||
      found->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto& operands = found->second.instruction->operands();
  return (operands[0] == induction && isOne(operands[1])) ||
         (operands[1] == induction && isOne(operands[0]));
}

std::optional<ScalarLoop> findScalarLoop(
    const Function& function, const CFG& cfg, const DefinitionMap& definitions,
    const std::string& headerName, const std::string& expectedExit) {
  const auto* header = findBlock(function, cfg, headerName);
  if (header == nullptr || header->terminator() == nullptr ||
      header->terminator()->opcode() != "br" ||
      header->terminator()->operands().size() != 3 ||
      header->terminator()->operands()[2] != expectedExit) {
    return std::nullopt;
  }
  const auto& branch = header->terminator()->operands();
  const auto condition = definitions.find(branch[0]);
  if (condition == definitions.end() ||
      condition->second.block != headerName ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const auto indexLoad =
      definitions.find(condition->second.instruction->operands()[0]);
  if (indexLoad == definitions.end() || indexLoad->second.block != headerName ||
      indexLoad->second.instruction->opcode() != "load" ||
      indexLoad->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const std::string slot = indexLoad->second.instruction->operands()[1];
  const std::string bodyName = branch[1];
  const auto* body = findBlock(function, cfg, bodyName);
  if (body == nullptr || body->terminator() == nullptr ||
      body->terminator()->opcode() != "br" ||
      body->terminator()->operands().size() != 1 ||
      body->terminator()->operands()[0] != headerName) {
    return std::nullopt;
  }

  std::optional<std::string> bodyIndex;
  std::size_t updates = 0;
  for (const auto& instruction : body->instructions()) {
    if (!isStoreTo(instruction, slot)) continue;
    const auto add = definitions.find(instruction.operands()[1]);
    if (add == definitions.end() || add->second.block != bodyName ||
        add->second.instruction->opcode() != "add" ||
        add->second.instruction->operands().size() != 2) {
      return std::nullopt;
    }
    const auto& operands = add->second.instruction->operands();
    std::string loaded;
    if (isOne(operands[0])) {
      loaded = operands[1];
    } else if (isOne(operands[1])) {
      loaded = operands[0];
    } else {
      return std::nullopt;
    }
    const auto load = definitions.find(loaded);
    if (load == definitions.end() || load->second.block != bodyName ||
        !isLoadFrom(*load->second.instruction, slot)) {
      return std::nullopt;
    }
    bodyIndex = loaded;
    ++updates;
  }
  if (updates != 1 || !bodyIndex.has_value()) return std::nullopt;

  const auto headerFound = cfg.index.find(headerName);
  if (headerFound == cfg.index.end() ||
      cfg.predecessors[headerFound->second].size() != 2) {
    return std::nullopt;
  }
  return ScalarLoop{headerName,
                    bodyName,
                    expectedExit,
                    slot,
                    indexLoad->second.instruction->result(),
                    *bodyIndex,
                    condition->second.instruction->operands()[1]};
}

std::optional<LoopNest> findLoopNest(const Function& function, const CFG& cfg,
                                     const DefinitionMap& definitions,
                                     const std::string& headerName) {
  const auto* header = findBlock(function, cfg, headerName);
  if (header == nullptr || header->terminator() == nullptr ||
      header->terminator()->opcode() != "br" ||
      header->terminator()->operands().size() != 3) {
    return std::nullopt;
  }
  const auto& branch = header->terminator()->operands();
  const auto condition = definitions.find(branch[0]);
  if (condition == definitions.end() ||
      condition->second.block != headerName ||
      condition->second.instruction->opcode() != "icmp.slt" ||
      condition->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  const std::string induction = condition->second.instruction->operands()[0];
  const auto inductionDefinition = definitions.find(induction);
  if (inductionDefinition == definitions.end() ||
      inductionDefinition->second.block != headerName ||
      inductionDefinition->second.instruction->opcode() != "phi" ||
      inductionDefinition->second.instruction->operands().size() != 5) {
    return std::nullopt;
  }
  const auto& phi = inductionDefinition->second.instruction->operands();
  if (phi[0] != "int") return std::nullopt;

  std::optional<std::string> latch;
  std::optional<std::string> preheader;
  for (std::size_t pair = 1; pair < phi.size(); pair += 2) {
    const auto* incoming = findBlock(function, cfg, phi[pair + 1]);
    if (incoming != nullptr && incoming->terminator() != nullptr &&
        incoming->terminator()->opcode() == "br" &&
        incoming->terminator()->operands().size() == 1 &&
        incoming->terminator()->operands()[0] == headerName &&
        isUnitIncrement(phi[pair], induction, definitions, phi[pair + 1])) {
      if (latch.has_value()) return std::nullopt;
      latch = phi[pair + 1];
    } else if (isZero(phi[pair])) {
      if (preheader.has_value()) return std::nullopt;
      preheader = phi[pair + 1];
    }
  }
  if (!latch.has_value() || !preheader.has_value()) return std::nullopt;

  const std::string bodyName = branch[1];
  const std::string exitName = branch[2];
  const auto* body = findBlock(function, cfg, bodyName);
  if (body == nullptr || body->terminator() == nullptr ||
      body->terminator()->opcode() != "br" ||
      body->terminator()->operands().size() != 1) {
    return std::nullopt;
  }
  const auto inner = findScalarLoop(function, cfg, definitions,
                                    body->terminator()->operands()[0], *latch);
  if (!inner.has_value()) return std::nullopt;

  bool initializesCounter = false;
  for (const auto& instruction : body->instructions()) {
    if (isStoreTo(instruction, inner->counterSlot) &&
        isZero(instruction.operands()[1])) {
      initializesCounter = true;
    }
    if (instruction.opcode() == "call") return std::nullopt;
  }
  if (!initializesCounter) return std::nullopt;

  return LoopNest{headerName,
                  bodyName,
                  exitName,
                  *latch,
                  induction,
                  condition->second.instruction->operands()[1],
                  *inner};
}

std::optional<std::pair<std::string, std::string>>
nextLoopHeaderThroughEmptyBlocks(const Function& function, const CFG& cfg,
                                 const DefinitionMap& definitions,
                                 const std::string& start) {
  std::string current = start;
  std::unordered_set<std::string> visited;
  while (visited.insert(current).second) {
    if (findLoopNest(function, cfg, definitions, current).has_value()) {
      return std::pair{current, std::string{}};
    }
    const auto* block = findBlock(function, cfg, current);
    if (block == nullptr || block->instructions().size() != 1 ||
        block->terminator() == nullptr ||
        block->terminator()->opcode() != "br" ||
        block->terminator()->operands().size() != 1) {
      return std::nullopt;
    }
    const std::string redirect = current;
    current = block->terminator()->operands()[0];
    if (findLoopNest(function, cfg, definitions, current).has_value()) {
      return std::pair{current, redirect};
    }
  }
  return std::nullopt;
}

std::optional<PointerPath> pointerPath(
    const std::string& value, const DefinitionMap& definitions,
    std::unordered_set<std::string>& visiting) {
  if (!value.empty() && value.front() == '@') {
    return PointerPath{value, {}};
  }
  if (value.empty() || value.front() != '%' || !visiting.insert(value).second) {
    return std::nullopt;
  }
  const auto found = definitions.find(value);
  if (found == definitions.end()) return std::nullopt;
  const auto& instruction = *found->second.instruction;
  if ((instruction.opcode() != "getelementptr" &&
       instruction.opcode() != "getelementptr.inbounds") ||
      instruction.operands().size() != 2) {
    return std::nullopt;
  }
  auto path = pointerPath(instruction.operands()[0], definitions, visiting);
  if (!path.has_value()) return std::nullopt;
  path->indices.push_back(instruction.operands()[1]);
  return path;
}

std::optional<PointerPath> pointerPath(const std::string& value,
                                       const DefinitionMap& definitions) {
  std::unordered_set<std::string> visiting;
  return pointerPath(value, definitions, visiting);
}

bool sameElementPath(const PointerPath& producer, const PointerPath& consumer,
                     const LoopNest& producerLoop,
                     const LoopNest& consumerLoop,
                     const DefinitionMap& definitions) {
  const auto loadsCounter = [&](const std::string& value,
                                const ScalarLoop& loop) {
    const auto found = definitions.find(value);
    return found != definitions.end() &&
           isLoadFrom(*found->second.instruction, loop.counterSlot);
  };
  if (producer.base != consumer.base ||
      producer.indices.size() != consumer.indices.size() ||
      producer.indices.size() < 2 ||
      producer.indices[producer.indices.size() - 2] !=
          producerLoop.induction ||
      !loadsCounter(producer.indices.back(), producerLoop.inner) ||
      consumer.indices[consumer.indices.size() - 2] !=
          consumerLoop.induction ||
      !loadsCounter(consumer.indices.back(), consumerLoop.inner)) {
    return false;
  }
  for (std::size_t index = 0; index < producer.indices.size(); ++index) {
    std::string normalized = consumer.indices[index];
    if (normalized == consumerLoop.induction) {
      normalized = producerLoop.induction;
    } else if (loadsCounter(normalized, consumerLoop.inner)) {
      if (!loadsCounter(producer.indices[index], producerLoop.inner)) {
        return false;
      }
      continue;
    }
    if (producer.indices[index] != normalized) return false;
  }
  return true;
}

std::unordered_set<std::string> scalarAllocasIn(
    const Function& function, const CFG& cfg,
    const std::vector<std::string>& blocks) {
  std::unordered_set<std::string> slots;
  for (const auto& name : blocks) {
    const auto* block = findBlock(function, cfg, name);
    if (block == nullptr) continue;
    for (const auto& instruction : block->instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult() &&
          instruction.operands().size() == 1 &&
          instruction.operands()[0].find('[') == std::string::npos) {
        slots.insert(instruction.result());
      }
    }
  }
  return slots;
}

bool localSlotsDoNotEscape(const Function& function,
                           const std::unordered_set<std::string>& slots) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      const auto& operands = instruction.operands();
      for (std::size_t index = 0; index < operands.size(); ++index) {
        if (slots.find(operands[index]) == slots.end()) continue;
        if (instruction.opcode() == "load" && index == 1) continue;
        if (instruction.opcode() == "store" && index == 2) continue;
        return false;
      }
    }
  }
  return true;
}

bool hasCallOrUnknownTerminator(const Function& function, const CFG& cfg,
                                const LoopNest& loop) {
  const std::vector<std::string> blocks{
      loop.header, loop.body, loop.inner.header, loop.inner.body, loop.latch};
  for (const auto& name : blocks) {
    const auto* block = findBlock(function, cfg, name);
    if (block == nullptr) return true;
    for (const auto& instruction : block->instructions()) {
      if (instruction.opcode() == "call" || instruction.opcode() == "ret") {
        return true;
      }
      if (!instruction.hasResult() && instruction.opcode() != "store" &&
          instruction.opcode() != "br") {
        return true;
      }
    }
  }
  return false;
}

bool isPureScalarOpcode(const std::string& opcode,
                        const std::vector<std::string>& operands) {
  static const std::unordered_set<std::string> pure{
      "add",       "sub",       "mul",       "sdiv",     "udiv",
      "srem",      "urem",      "and",       "or",       "xor",
      "shl",       "lshr",      "ashr",      "icmp.eq",  "icmp.ne",
      "icmp.slt",  "icmp.sle",  "icmp.sgt",  "icmp.sge", "icmp.ult",
      "icmp.ule",  "icmp.ugt",  "icmp.uge",  "select",   "zext",
      "sext",      "trunc"};
  if (pure.find(opcode) == pure.end()) return false;
  if ((opcode == "sdiv" || opcode == "udiv" || opcode == "srem" ||
       opcode == "urem") &&
      operands.size() != 2) {
    return false;
  }
  if (opcode == "sdiv" || opcode == "udiv" || opcode == "srem" ||
      opcode == "urem") {
    long long divisor = 0;
    if (!detail::parseIntegerLiteral(operands[1], divisor) || divisor == 0 ||
        ((opcode == "sdiv" || opcode == "srem") && divisor == -1)) {
      return false;
    }
  }
  return true;
}

bool resultUsedOutside(const Function& function, const std::string& result,
                       const std::unordered_set<std::string>& removedBlocks) {
  for (const auto& block : function.blocks()) {
    if (removedBlocks.find(block.name()) != removedBlocks.end()) continue;
    for (const auto& instruction : block.instructions()) {
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    result) != instruction.operands().end()) {
        return true;
      }
    }
  }
  return false;
}

std::string resolveAlias(
    const std::string& value,
    const std::unordered_map<std::string, std::string>& aliases) {
  std::string resolved = value;
  std::unordered_set<std::string> visited;
  while (visited.insert(resolved).second) {
    const auto found = aliases.find(resolved);
    if (found == aliases.end()) break;
    resolved = found->second;
  }
  return resolved;
}

std::optional<FusionCandidate> findCandidate(
    const Function& function, const CFG& cfg, const DefinitionMap& definitions,
    const std::string& producerHeader) {
  const auto miss = [&](const char* reason)
      -> std::optional<FusionCandidate> {
    (void)reason;
    return std::nullopt;
  };
  const auto producer =
      findLoopNest(function, cfg, definitions, producerHeader);
  if (!producer.has_value()) return miss("producer shape");
  const auto adjacent = nextLoopHeaderThroughEmptyBlocks(
      function, cfg, definitions, producer->exit);
  if (!adjacent.has_value()) return miss("consumer shape");
  const auto consumer =
      findLoopNest(function, cfg, definitions, adjacent->first);
  if (!consumer.has_value()) return miss("consumer shape");
  if (producer->bound != consumer->bound) return miss("outer bound mismatch");
  if (producer->inner.bound != consumer->inner.bound) {
    return miss("inner bound mismatch");
  }
  if (hasCallOrUnknownTerminator(function, cfg, *producer)) {
    return miss("producer call or return");
  }
  if (hasCallOrUnknownTerminator(function, cfg, *consumer)) {
    return miss("consumer call or return");
  }

  const auto consumerHeaderFound = cfg.index.find(consumer->header);
  const std::string entryRedirect =
      adjacent->second.empty() ? producer->header : adjacent->second;
  const auto hasExpectedPredecessor = [&](const std::string& name) {
    if (consumerHeaderFound == cfg.index.end()) return false;
    const auto& predecessors = cfg.predecessors[consumerHeaderFound->second];
    return std::find(predecessors.begin(), predecessors.end(), name) !=
           predecessors.end();
  };
  if (consumerHeaderFound == cfg.index.end() ||
      cfg.predecessors[consumerHeaderFound->second].size() != 2 ||
      !hasExpectedPredecessor(entryRedirect) ||
      !hasExpectedPredecessor(consumer->latch)) {
    return miss("not adjacent");
  }

  const std::vector<std::string> producerBlocks{
      producer->body, producer->inner.body};
  const std::vector<std::string> consumerBlocks{
      consumer->body, consumer->inner.body};
  auto producerLocals = scalarAllocasIn(function, cfg, producerBlocks);
  auto consumerLocals = scalarAllocasIn(function, cfg, consumerBlocks);
  producerLocals.insert(producer->inner.counterSlot);
  consumerLocals.insert(consumer->inner.counterSlot);
  if (!localSlotsDoNotEscape(function, producerLocals) ||
      !localSlotsDoNotEscape(function, consumerLocals)) {
    return miss("local escape");
  }

  const auto* producerBody = findBlock(function, cfg, producer->inner.body);
  const auto* consumerBody = findBlock(function, cfg, consumer->inner.body);
  if (producerBody == nullptr || consumerBody == nullptr) {
    return miss("missing body");
  }

  const Instruction* producerStore = nullptr;
  std::vector<const Instruction*> producerLoads;
  for (const auto& instruction : producerBody->instructions()) {
    if (instruction.opcode() == "store" &&
        instruction.operands().size() == 3 &&
        producerLocals.find(instruction.operands()[2]) ==
            producerLocals.end()) {
      if (producerStore != nullptr) return miss("multiple producer stores");
      producerStore = &instruction;
    } else if (instruction.opcode() == "load" &&
               instruction.operands().size() == 2 &&
               producerLocals.find(instruction.operands()[1]) ==
                   producerLocals.end()) {
      producerLoads.push_back(&instruction);
    } else if (instruction.opcode() == "call") {
      return miss("producer call");
    }
  }
  if (producerStore == nullptr || producerStore->operands()[0] != "int") {
    return miss("no i32 producer store");
  }
  for (const auto* load : producerLoads) {
    if (load->operands()[0] != "int") {
      return miss("non-i32 producer load");
    }
  }
  const auto producerPath =
      pointerPath(producerStore->operands()[2], definitions);
  if (!producerPath.has_value() || producerPath->base.empty() ||
      producerPath->base.front() != '@') {
    return miss("producer address");
  }
  for (const auto* load : producerLoads) {
    const auto path = pointerPath(load->operands()[1], definitions);
    if (!path.has_value() || path->base == producerPath->base) {
      return miss("producer reads destination or unknown object");
    }
  }

  const Instruction* consumerLoad = nullptr;
  const Instruction* consumerStore = nullptr;
  for (const auto& instruction : consumerBody->instructions()) {
    if (instruction.opcode() == "store" &&
        instruction.operands().size() == 3 &&
        consumerLocals.find(instruction.operands()[2]) ==
            consumerLocals.end()) {
      if (consumerStore != nullptr) return miss("multiple consumer stores");
      consumerStore = &instruction;
    } else if (instruction.opcode() == "load" &&
               instruction.operands().size() == 2 &&
               consumerLocals.find(instruction.operands()[1]) ==
                   consumerLocals.end()) {
      if (consumerLoad != nullptr) return miss("multiple consumer loads");
      consumerLoad = &instruction;
    } else if (instruction.opcode() == "call") {
      return miss("consumer call");
    }
  }
  if (consumerLoad == nullptr || consumerStore == nullptr) {
    return miss("consumer memory shape");
  }
  if (consumerLoad->operands()[0] != "int" ||
      consumerStore->operands()[0] != "int") {
    return miss("non-i32 consumer memory");
  }

  std::unordered_map<std::string, std::string> localValues;
  std::unordered_map<std::string, std::string> consumerAliases;
  for (const auto& instruction : consumerBody->instructions()) {
    if (instruction.opcode() == "store" &&
        instruction.operands().size() == 3 &&
        consumerLocals.find(instruction.operands()[2]) !=
            consumerLocals.end() &&
        instruction.operands()[2] != consumer->inner.counterSlot) {
      localValues[instruction.operands()[2]] =
          resolveAlias(instruction.operands()[1], consumerAliases);
    } else if (instruction.opcode() == "load" && instruction.hasResult() &&
               instruction.operands().size() == 2) {
      const auto current = localValues.find(instruction.operands()[1]);
      if (current != localValues.end()) {
        consumerAliases[instruction.result()] =
            resolveAlias(current->second, consumerAliases);
      }
    }
  }
  const auto consumerLoadPath =
      pointerPath(consumerLoad->operands()[1], definitions);
  const auto consumerStorePath =
      pointerPath(consumerStore->operands()[2], definitions);
  if (!consumerLoadPath.has_value() || !consumerStorePath.has_value() ||
      !sameElementPath(*producerPath, *consumerLoadPath, *producer,
                       *consumer, definitions) ||
      !sameElementPath(*producerPath, *consumerStorePath, *producer,
                       *consumer, definitions)) {
    return miss("element path mismatch");
  }

  std::unordered_set<std::string> dependencyValues{consumerLoad->result()};
  std::function<bool(const std::string&)> validateValue =
      [&](const std::string& value) {
        const std::string resolved = resolveAlias(value, consumerAliases);
        if (resolved == consumerLoad->result() || resolved.empty() ||
            resolved.front() != '%') {
          return true;
        }
        if (!dependencyValues.insert(resolved).second) return true;
        const auto found = definitions.find(resolved);
        if (found == definitions.end() ||
            found->second.block != consumer->inner.body ||
            !isPureScalarOpcode(found->second.instruction->opcode(),
                                found->second.instruction->operands())) {
          return false;
        }
        for (const auto& operand : found->second.instruction->operands()) {
          if (!validateValue(operand)) return false;
        }
        return true;
      };
  if (!validateValue(consumerStore->operands()[1])) {
    return miss("consumer value is not safe pure SSA");
  }

  for (const auto& instruction : consumerBody->instructions()) {
    if (!instruction.hasResult() ||
        dependencyValues.find(instruction.result()) != dependencyValues.end() ||
        instruction.opcode() == "alloca" ||
        (instruction.opcode() == "load" &&
         instruction.operands().size() == 2 &&
         consumerLocals.find(instruction.operands()[1]) !=
             consumerLocals.end())) {
      continue;
    }
    if (instruction.opcode() == "getelementptr" ||
        instruction.opcode() == "getelementptr.inbounds") {
      continue;
    }
    const bool isCounterAdd =
        isUnitIncrement(instruction.result(), consumer->inner.bodyIndex,
                        definitions, consumer->inner.body);
    if (!isCounterAdd) return miss("extra consumer result");
  }

  const std::unordered_set<std::string> removedBlocks{
      consumer->header, consumer->body, consumer->inner.header,
      consumer->inner.body, consumer->latch};
  for (const auto& name : removedBlocks) {
    const auto* block = findBlock(function, cfg, name);
    if (block == nullptr) return miss("missing removed block");
    for (const auto& instruction : block->instructions()) {
      if (instruction.hasResult() &&
          resultUsedOutside(function, instruction.result(), removedBlocks)) {
        return miss("removed value has outside use");
      }
    }
  }

  return FusionCandidate{*producer,
                         *consumer,
                         entryRedirect,
                         producerStore->operands()[2],
                         producerStore->operands()[1],
                         consumerLoad->result(),
                         consumerStore->operands()[1],
                         consumerStore->operands()[0],
                         std::move(consumerAliases)};
}

std::unordered_set<std::string> collectValueNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  return names;
}

std::string freshValue(std::unordered_set<std::string>& names,
                       std::size_t& counter) {
  while (true) {
    const std::string candidate =
        "%adjacent.fused." + std::to_string(counter++);
    if (names.insert(candidate).second) return candidate;
  }
}

bool applyFusion(Function& function, const DefinitionMap& definitions,
                 const FusionCandidate& candidate) {
  auto names = collectValueNames(function);
  std::size_t counter = 0;
  std::unordered_map<std::string, std::string> cloned{
      {candidate.consumerLoad, candidate.producerStoredValue}};
  std::vector<Instruction> computation;
  std::function<std::optional<std::string>(const std::string&)> cloneValue =
      [&](const std::string& value) -> std::optional<std::string> {
    const std::string resolved =
        resolveAlias(value, candidate.consumerAliases);
    if (const auto found = cloned.find(resolved); found != cloned.end()) {
      return found->second;
    }
    if (resolved.empty() || resolved.front() != '%') return resolved;
    const auto definition = definitions.find(resolved);
    if (definition == definitions.end() ||
        definition->second.block != candidate.consumer.inner.body ||
        !isPureScalarOpcode(definition->second.instruction->opcode(),
                            definition->second.instruction->operands())) {
      return std::nullopt;
    }
    std::vector<std::string> operands;
    for (const auto& operand : definition->second.instruction->operands()) {
      const auto rewritten = cloneValue(operand);
      if (!rewritten.has_value()) return std::nullopt;
      operands.push_back(*rewritten);
    }
    const std::string result = freshValue(names, counter);
    computation.emplace_back(definition->second.instruction->opcode(),
                             std::move(operands), result);
    cloned.emplace(resolved, result);
    return result;
  };
  const auto finalValue = cloneValue(candidate.consumerStoredValue);
  if (!finalValue.has_value()) return false;

  auto* producerBody = findBlock(function, candidate.producer.inner.body);
  auto* redirectBlock = findBlock(function, candidate.entryRedirectBlock);
  auto* consumerExit = findBlock(function, candidate.consumer.exit);
  if (producerBody == nullptr || redirectBlock == nullptr ||
      consumerExit == nullptr || redirectBlock->terminator() == nullptr) {
    return false;
  }
  auto redirectInstructions = redirectBlock->instructions();
  auto& branch = redirectInstructions.back();
  auto branchOperands = branch.operands();
  if (branch.opcode() != "br" ||
      (branchOperands.size() != 1 && branchOperands.size() != 3) ||
      branchOperands.back() != candidate.consumer.header) {
    return false;
  }

  std::vector<Instruction> rewrittenBody;
  bool replacedStore = false;
  for (const auto& instruction : producerBody->instructions()) {
    if (!replacedStore && instruction.opcode() == "store" &&
        instruction.operands().size() == 3 &&
        instruction.operands()[2] == candidate.producerStoreAddress) {
      rewrittenBody.insert(rewrittenBody.end(), computation.begin(),
                           computation.end());
      rewrittenBody.emplace_back(
          "store",
          std::vector<std::string>{candidate.consumerStoreType, *finalValue,
                                   candidate.producerStoreAddress});
      replacedStore = true;
    } else {
      rewrittenBody.push_back(instruction);
    }
  }
  if (!replacedStore) return false;
  producerBody->instructions() = std::move(rewrittenBody);

  branchOperands.back() = candidate.consumer.exit;
  branch = Instruction{"br", std::move(branchOperands)};
  redirectBlock->instructions() = std::move(redirectInstructions);

  std::vector<Instruction> rewrittenExit;
  for (const auto& instruction : consumerExit->instructions()) {
    if (instruction.opcode() != "phi") {
      rewrittenExit.push_back(instruction);
      continue;
    }
    auto operands = instruction.operands();
    for (std::size_t index = 2; index < operands.size(); index += 2) {
      if (operands[index] == candidate.consumer.header) {
        operands[index] = candidate.entryRedirectBlock;
      }
    }
    rewrittenExit.emplace_back("phi", std::move(operands),
                               instruction.result());
  }
  consumerExit->instructions() = std::move(rewrittenExit);

  const std::unordered_set<std::string> removed{
      candidate.consumer.header, candidate.consumer.body,
      candidate.consumer.inner.header, candidate.consumer.inner.body,
      candidate.consumer.latch};
  auto& blocks = function.blocks();
  blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                              [&](const BasicBlock& block) {
                                return removed.find(block.name()) !=
                                       removed.end();
                              }),
               blocks.end());
  return true;
}

bool fuseOne(Function& function) {
  const CFG cfg = buildCFG(function);
  const DefinitionMap definitions = buildDefinitions(function);
  for (const auto& block : function.blocks()) {
    const auto candidate =
        findCandidate(function, cfg, definitions, block.name());
    if (candidate.has_value()) {
      return applyFusion(function, definitions, *candidate);
    }
  }
  return false;
}

}  // namespace

void AdjacentProducerConsumerLoopFusionPass::run(
    compiler::ir::core::Module& module) {
  if (std::getenv(
          "COMPILER_DISABLE_ADJACENT_PRODUCER_CONSUMER_LOOP_FUSION") !=
      nullptr) {
    return;
  }
  for (auto& function : module.functions()) {
    while (fuseOne(function)) {
    }
  }
}

}  // namespace compiler::midend::pass
