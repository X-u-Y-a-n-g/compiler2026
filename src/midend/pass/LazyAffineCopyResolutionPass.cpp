#include "../../../include/compiler/midend/pass/LazyAffineCopyResolutionPass.h"

#include <algorithm>
#include <cstddef>
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
using compiler::ir::core::Module;

struct Definition {
  const Instruction *instruction = nullptr;
  std::size_t block = 0;
};
using DefinitionMap = std::unordered_map<std::string, Definition>;

struct PointerAccess {
  std::string base;
  std::string index;
};

struct SimpleLoop {
  std::string header;
  std::string body;
  std::string preheader;
  std::string backedge;
  std::string exit;
  std::string induction;
  std::string bound;
  std::size_t headerIndex = 0;
  std::size_t bodyIndex = 0;
};

struct CopyKernel {
  std::size_t functionIndex = 0;
};

struct InitializationPath {
  std::string value;
  std::vector<std::pair<std::string, bool>> conditions;
};

struct InitializationProducer {
  SimpleLoop loop;
  std::vector<InitializationPath> paths;
  std::string defaultValue;
  std::unordered_set<std::size_t> region;
};

struct Candidate {
  std::size_t functionIndex = 0;
  SimpleLoop calls;
  SimpleLoop consumer;
  std::string count;
  std::string length;
  std::string data;
  std::string parameters;
  std::string consumerPointer;
  std::size_t dataCapacity = 0;
  std::size_t parameterCapacity = 0;
  std::string targetLoad;
  std::optional<InitializationProducer> initializer;
};

struct CFG {
  std::unordered_map<std::string, std::size_t> indices;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<std::unordered_set<std::size_t>> dominators;
};

DefinitionMap definitionsOf(const Function &function) {
  DefinitionMap result;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto &instruction : function.blocks()[block].instructions()) {
      if (instruction.hasResult()) {
        result.emplace(instruction.result(), Definition{&instruction, block});
      }
    }
  }
  return result;
}

std::unordered_map<std::string, std::size_t>
blockIndices(const Function &function) {
  std::unordered_map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    result.emplace(function.blocks()[index].name(), index);
  }
  return result;
}

std::vector<std::string> branchTargets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
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

CFG buildCFG(const Function &function) {
  CFG cfg;
  cfg.indices = blockIndices(function);
  const auto count = function.blocks().size();
  cfg.successors.resize(count);
  cfg.predecessors.resize(count);
  cfg.dominators.resize(count);
  for (std::size_t block = 0; block < count; ++block) {
    for (const auto &target : branchTargets(function.blocks()[block])) {
      const auto found = cfg.indices.find(target);
      if (found == cfg.indices.end())
        continue;
      cfg.successors[block].push_back(found->second);
      cfg.predecessors[found->second].push_back(block);
    }
  }
  if (count == 0)
    return cfg;
  cfg.dominators[0].insert(0);
  for (std::size_t block = 1; block < count; ++block) {
    for (std::size_t candidate = 0; candidate < count; ++candidate) {
      cfg.dominators[block].insert(candidate);
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block = 1; block < count; ++block) {
      std::unordered_set<std::size_t> next;
      if (!cfg.predecessors[block].empty()) {
        next = cfg.dominators[cfg.predecessors[block].front()];
        for (std::size_t index = 1; index < cfg.predecessors[block].size();
             ++index) {
          const auto &other = cfg.dominators[cfg.predecessors[block][index]];
          for (auto iterator = next.begin(); iterator != next.end();) {
            if (other.find(*iterator) == other.end()) {
              iterator = next.erase(iterator);
            } else {
              ++iterator;
            }
          }
        }
      }
      next.insert(block);
      if (next != cfg.dominators[block]) {
        cfg.dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return cfg;
}

bool dominates(const CFG &cfg, std::size_t first, std::size_t second) {
  return second < cfg.dominators.size() &&
         cfg.dominators[second].find(first) != cfg.dominators[second].end();
}

std::unordered_set<std::size_t> reachableFrom(const Function &function,
                                              std::size_t start) {
  const auto indices = blockIndices(function);
  std::unordered_set<std::size_t> reachable{start};
  std::vector<std::size_t> worklist{start};
  while (!worklist.empty()) {
    const auto block = worklist.back();
    worklist.pop_back();
    const auto *terminator = function.blocks()[block].terminator();
    if (terminator == nullptr)
      continue;
    for (const auto &operand : terminator->operands()) {
      const auto successor = indices.find(operand);
      if (successor != indices.end() &&
          reachable.insert(successor->second).second) {
        worklist.push_back(successor->second);
      }
    }
  }
  return reachable;
}

bool isInteger(const std::string &value, long long expected) {
  long long parsed = 0;
  return detail::parseIntegerLiteral(value, parsed) && parsed == expected;
}

bool isGep(const Instruction &instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "getelementptr.linear" ||
         instruction.opcode() == "gep" || instruction.opcode() == "gep.linear";
}

std::optional<PointerAccess> pointerAccess(const std::string &pointer,
                                           const DefinitionMap &definitions) {
  const auto found = definitions.find(pointer);
  if (found == definitions.end() || !isGep(*found->second.instruction) ||
      found->second.instruction->operands().size() != 2) {
    return std::nullopt;
  }
  std::string base = found->second.instruction->operands()[0];
  const std::string index = found->second.instruction->operands()[1];
  std::unordered_set<std::string> visiting;
  while (!base.empty() && base.front() == '%') {
    if (!visiting.insert(base).second)
      return std::nullopt;
    const auto definition = definitions.find(base);
    if (definition == definitions.end() ||
        definition->second.instruction->opcode() != "cast" ||
        definition->second.instruction->operands().empty()) {
      break;
    }
    base = definition->second.instruction->operands().front();
  }
  return PointerAccess{base, index};
}

std::string pointerRoot(const std::string &pointer,
                        const DefinitionMap &definitions) {
  std::string root = pointer;
  std::unordered_set<std::string> visiting;
  while (!root.empty() && root.front() == '%') {
    if (!visiting.insert(root).second)
      return {};
    const auto definition = definitions.find(root);
    if (definition == definitions.end())
      break;
    const auto &instruction = *definition->second.instruction;
    if ((instruction.opcode() != "cast" && !isGep(instruction)) ||
        instruction.operands().empty()) {
      break;
    }
    root = instruction.operands().front();
  }
  return root;
}

bool matchBinary(const std::string &value, const std::string &opcode,
                 const std::string &lhs, const std::string &rhs,
                 const DefinitionMap &definitions) {
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != opcode ||
      found->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto &operands = found->second.instruction->operands();
  return (operands[0] == lhs && operands[1] == rhs) ||
         (operands[0] == rhs && operands[1] == lhs);
}

bool matchProductPlus(const std::string &value, const std::string &lhs,
                      const std::string &rhs, const std::string &addend,
                      const DefinitionMap &definitions) {
  const auto found = definitions.find(value);
  if (found == definitions.end() ||
      found->second.instruction->opcode() != "add" ||
      found->second.instruction->operands().size() != 2) {
    return false;
  }
  const auto &operands = found->second.instruction->operands();
  return (operands[0] == addend &&
          matchBinary(operands[1], "mul", lhs, rhs, definitions)) ||
         (operands[1] == addend &&
          matchBinary(operands[0], "mul", lhs, rhs, definitions));
}

std::vector<SimpleLoop> simpleLoops(const Function &function,
                                    const DefinitionMap &definitions) {
  std::vector<SimpleLoop> result;
  const auto indices = blockIndices(function);
  for (std::size_t headerIndex = 0; headerIndex < function.blocks().size();
       ++headerIndex) {
    const auto &header = function.blocks()[headerIndex];
    const auto *branch = header.terminator();
    if (branch == nullptr || branch->opcode() != "br" ||
        branch->operands().size() != 3) {
      continue;
    }
    const auto condition = definitions.find(branch->operands()[0]);
    if (condition == definitions.end() ||
        condition->second.block != headerIndex ||
        condition->second.instruction->opcode() != "icmp.slt" ||
        condition->second.instruction->operands().size() != 2) {
      continue;
    }
    const std::string induction = condition->second.instruction->operands()[0];
    const std::string bound = condition->second.instruction->operands()[1];
    const auto phi = definitions.find(induction);
    if (phi == definitions.end() || phi->second.block != headerIndex ||
        phi->second.instruction->opcode() != "phi" ||
        phi->second.instruction->operands().size() != 5) {
      continue;
    }
    std::string preheader;
    std::string backedge;
    std::string increment;
    const auto &phiOperands = phi->second.instruction->operands();
    for (std::size_t index = 1; index + 1 < phiOperands.size(); index += 2) {
      if (isInteger(phiOperands[index], 0)) {
        preheader = phiOperands[index + 1];
      } else {
        increment = phiOperands[index];
        backedge = phiOperands[index + 1];
      }
    }
    const auto body = indices.find(branch->operands()[1]);
    const auto exit = indices.find(branch->operands()[2]);
    const auto back = indices.find(backedge);
    const auto incrementDefinition = definitions.find(increment);
    if (preheader.empty() || body == indices.end() || exit == indices.end() ||
        back == indices.end() || incrementDefinition == definitions.end() ||
        incrementDefinition->second.instruction->opcode() != "add" ||
        incrementDefinition->second.instruction->operands().size() != 2) {
      continue;
    }
    const auto &add = incrementDefinition->second.instruction->operands();
    const auto *backBranch = function.blocks()[back->second].terminator();
    if (!((add[0] == induction && isInteger(add[1], 1)) ||
          (add[1] == induction && isInteger(add[0], 1))) ||
        backBranch == nullptr || backBranch->operands().size() != 1 ||
        backBranch->operands()[0] != header.name()) {
      continue;
    }
    result.push_back(SimpleLoop{header.name(), branch->operands()[1], preheader,
                                backedge, branch->operands()[2], induction,
                                bound, headerIndex, body->second});
  }
  return result;
}

bool valueUnused(const Function &function, const Instruction &definition) {
  if (!definition.hasResult())
    return true;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (&instruction == &definition)
        continue;
      if (std::find(instruction.operands().begin(),
                    instruction.operands().end(),
                    definition.result()) != instruction.operands().end()) {
        return false;
      }
    }
  }
  return true;
}

std::optional<CopyKernel> analyzeKernel(const Function &function,
                                        std::size_t functionIndex) {
  if (function.parameters().size() != 3 ||
      function.parameters()[0].type != "int" ||
      (function.parameters()[1].type != "ptr" &&
       function.parameters()[1].type.find("[]") == std::string::npos) ||
      function.parameters()[2].type != "int") {
    return std::nullopt;
  }
  const std::string count = function.parameters()[0].name;
  const std::string data = function.parameters()[1].name;
  const std::string rowSize = function.parameters()[2].name;
  const auto definitions = definitionsOf(function);
  const auto loops = simpleLoops(function, definitions);
  std::unordered_set<std::string> localAllocas;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        localAllocas.insert(instruction.result());
      }
    }
  }
  std::string columnSize;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "call")
        return std::nullopt;
      if (instruction.opcode() == "sdiv" &&
          instruction.operands().size() == 2 &&
          instruction.operands()[0] == count &&
          instruction.operands()[1] == rowSize) {
        if (!columnSize.empty())
          return std::nullopt;
        columnSize = instruction.result();
      }
    }
  }
  if (columnSize.empty())
    return std::nullopt;

  const Instruction *load = nullptr;
  const Instruction *store = nullptr;
  std::size_t loadBlock = 0;
  std::size_t storeBlock = 0;
  PointerAccess loadAccess;
  PointerAccess storeAccess;
  for (std::size_t blockIndex = 0; blockIndex < function.blocks().size();
       ++blockIndex) {
    const auto &block = function.blocks()[blockIndex];
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "load" && instruction.opcode() != "store") {
        continue;
      }
      const auto &address = instruction.operands().back();
      const auto access = pointerAccess(address, definitions);
      const auto root = pointerRoot(address, definitions);
      if (localAllocas.find(root) != localAllocas.end()) {
        continue;
      }
      if (!access.has_value() || root != data) {
        return std::nullopt;
      }
      if (instruction.opcode() == "load") {
        if (load != nullptr)
          return std::nullopt;
        load = &instruction;
        loadBlock = blockIndex;
        loadAccess = *access;
      } else {
        if (store != nullptr)
          return std::nullopt;
        store = &instruction;
        storeBlock = blockIndex;
        storeAccess = *access;
      }
    }
  }
  if (load == nullptr || store == nullptr || store->operands().size() != 3 ||
      store->operands()[1] != load->result()) {
    return std::nullopt;
  }

  for (const auto &outer : loops) {
    if (outer.bound != columnSize)
      continue;
    for (const auto &inner : loops) {
      if (inner.header == outer.header || inner.body != inner.backedge ||
          loadBlock != inner.bodyIndex || storeBlock != inner.bodyIndex ||
          !matchProductPlus(loadAccess.index, outer.induction, rowSize,
                            inner.induction, definitions) ||
          !matchProductPlus(storeAccess.index, inner.induction, columnSize,
                            outer.induction, definitions)) {
        continue;
      }
      const auto bound = definitions.find(inner.bound);
      if (bound == definitions.end() ||
          bound->second.instruction->opcode() != "select" ||
          bound->second.instruction->operands().size() != 3) {
        continue;
      }
      const auto &select = bound->second.instruction->operands();
      if (select[2] != rowSize ||
          !matchBinary(select[1], "add", outer.induction, "1", definitions)) {
        continue;
      }
      const auto comparison = definitions.find(select[0]);
      if (comparison == definitions.end() ||
          comparison->second.instruction->opcode() != "icmp.slt" ||
          comparison->second.instruction->operands().size() != 2 ||
          comparison->second.instruction->operands()[0] != select[1] ||
          comparison->second.instruction->operands()[1] != rowSize) {
        continue;
      }
      return CopyKernel{functionIndex};
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> globalCapacity(const Module &module,
                                          const std::string &value) {
  if (value.empty() || value.front() != '@')
    return std::nullopt;
  for (const auto &global : module.globals()) {
    if ("@" + global.name() != value)
      continue;
    const auto open = global.type().find('[');
    const auto close = global.type().find(']', open);
    long long capacity = 0;
    if (open == std::string::npos || close == std::string::npos ||
        close <= open + 1) {
      return std::nullopt;
    }
    auto extent = global.type().substr(open + 1, close - open - 1);
    const auto separator = extent.find(" x ");
    if (separator != std::string::npos)
      extent.resize(separator);
    if (!detail::parseIntegerLiteral(extent, capacity) || capacity <= 0) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(capacity);
  }
  return std::nullopt;
}

bool isZeroConstant(const compiler::ir::core::Constant &constant) {
  if (constant.isAggregate()) {
    return std::all_of(constant.elements().begin(), constant.elements().end(),
                       isZeroConstant);
  }
  long long integer = 0;
  return detail::parseIntegerLiteral(constant.literal(), integer) &&
         integer == 0;
}

std::optional<std::string> uniformGlobalDefault(const Module &module,
                                                const std::string &value) {
  if (value.empty() || value.front() != '@')
    return std::nullopt;
  for (const auto &global : module.globals()) {
    if ("@" + global.name() != value)
      continue;
    if (isZeroConstant(global.initializer()))
      return std::string{"0"};
    return std::nullopt;
  }
  return std::nullopt;
}

bool isSafeInitializerOpcode(const std::string &opcode) {
  return opcode == "add" || opcode == "sub" || opcode == "mul" ||
         opcode == "and" || opcode == "or" || opcode == "xor" ||
         opcode == "select" || opcode.rfind("icmp.", 0) == 0;
}

bool initializerValueCloneable(const Function &function,
                               const DefinitionMap &definitions, const CFG &cfg,
                               const SimpleLoop &loop,
                               const std::unordered_set<std::size_t> &region,
                               const std::string &value,
                               std::unordered_map<std::string, bool> &memo,
                               std::unordered_set<std::string> &visiting) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(value, literal) || value == loop.induction) {
    return true;
  }
  const auto cached = memo.find(value);
  if (cached != memo.end())
    return cached->second;
  if (value.empty() || value.front() != '%')
    return false;
  const auto definition = definitions.find(value);
  if (definition == definitions.end()) {
    const bool parameter = std::any_of(
        function.parameters().begin(), function.parameters().end(),
        [&](const auto &candidate) { return candidate.name == value; });
    memo.emplace(value, parameter);
    return parameter;
  }
  if (region.find(definition->second.block) == region.end()) {
    const auto preheader = cfg.indices.find(loop.preheader);
    const bool invariant =
        preheader != cfg.indices.end() &&
        dominates(cfg, definition->second.block, preheader->second);
    memo.emplace(value, invariant);
    return invariant;
  }
  if (!visiting.insert(value).second ||
      !isSafeInitializerOpcode(definition->second.instruction->opcode())) {
    memo.emplace(value, false);
    return false;
  }
  bool cloneable = true;
  for (const auto &operand : definition->second.instruction->operands()) {
    if (!initializerValueCloneable(function, definitions, cfg, loop, region,
                                   operand, memo, visiting)) {
      cloneable = false;
      break;
    }
  }
  visiting.erase(value);
  memo.emplace(value, cloneable);
  return cloneable;
}

std::optional<InitializationProducer>
analyzeInitializationProducer(const Module &module, const Function &function,
                              const DefinitionMap &definitions,
                              const std::vector<SimpleLoop> &loops,
                              const SimpleLoop &calls, const std::string &count,
                              const std::string &data) {
  const auto defaultValue = uniformGlobalDefault(module, data);
  if (!defaultValue.has_value())
    return std::nullopt;
  const auto cfg = buildCFG(function);

  for (const auto &loop : loops) {
    if (loop.header == calls.header || loop.bound != count) {
      continue;
    }
    bool reachesCalls =
        loop.exit == calls.header && calls.preheader == loop.header;
    if (!reachesCalls && loop.exit == calls.preheader) {
      const auto exit = cfg.indices.find(loop.exit);
      if (exit != cfg.indices.end()) {
        const auto &instructions =
            function.blocks()[exit->second].instructions();
        reachesCalls = instructions.size() == 1 &&
                       instructions.back().opcode() == "br" &&
                       instructions.back().operands().size() == 1 &&
                       instructions.back().operands()[0] == calls.header;
      }
    }
    if (!reachesCalls)
      continue;
    const auto backedge = cfg.indices.find(loop.backedge);
    if (backedge == cfg.indices.end())
      continue;

    std::vector<InitializationPath> paths;
    std::unordered_set<std::size_t> region;
    bool invalid = false;
    const std::size_t kMaximumPaths = 32;
    const auto walk = [&](auto &&self, std::size_t block,
                          std::unordered_set<std::size_t> visited,
                          std::vector<std::pair<std::string, bool>> conditions,
                          std::optional<std::string> storedValue) -> void {
      if (invalid || paths.size() >= kMaximumPaths ||
          !visited.insert(block).second) {
        invalid = true;
        return;
      }
      region.insert(block);
      const auto &basicBlock = function.blocks()[block];
      const auto *terminator = basicBlock.terminator();
      if (terminator == nullptr || terminator->opcode() != "br") {
        invalid = true;
        return;
      }
      for (const auto &instruction : basicBlock.instructions()) {
        if (&instruction == terminator)
          continue;
        if (instruction.opcode() == "store") {
          if (instruction.operands().size() != 3) {
            invalid = true;
            return;
          }
          const auto access =
              pointerAccess(instruction.operands().back(), definitions);
          if (!access.has_value() || access->base != data ||
              access->index != loop.induction) {
            invalid = true;
            return;
          }
          storedValue = instruction.operands()[1];
          continue;
        }
        if (isGep(instruction)) {
          const auto access = pointerAccess(instruction.result(), definitions);
          if (!access.has_value() || access->base != data ||
              access->index != loop.induction) {
            invalid = true;
            return;
          }
          continue;
        }
        if (!isSafeInitializerOpcode(instruction.opcode())) {
          invalid = true;
          return;
        }
      }

      if (block == backedge->second) {
        if (terminator->operands().size() != 1 ||
            terminator->operands()[0] != loop.header ||
            !storedValue.has_value()) {
          invalid = true;
          return;
        }
        paths.push_back(
            InitializationPath{*storedValue, std::move(conditions)});
        return;
      }
      if (terminator->operands().size() == 1) {
        const auto next = cfg.indices.find(terminator->operands()[0]);
        if (next == cfg.indices.end() || next->second == loop.headerIndex) {
          invalid = true;
          return;
        }
        self(self, next->second, std::move(visited), std::move(conditions),
             std::move(storedValue));
        return;
      }
      if (terminator->operands().size() != 3) {
        invalid = true;
        return;
      }
      const auto trueBlock = cfg.indices.find(terminator->operands()[1]);
      const auto falseBlock = cfg.indices.find(terminator->operands()[2]);
      if (trueBlock == cfg.indices.end() || falseBlock == cfg.indices.end()) {
        invalid = true;
        return;
      }
      auto trueConditions = conditions;
      trueConditions.emplace_back(terminator->operands()[0], true);
      conditions.emplace_back(terminator->operands()[0], false);
      self(self, trueBlock->second, visited, std::move(trueConditions),
           storedValue);
      self(self, falseBlock->second, std::move(visited), std::move(conditions),
           std::move(storedValue));
    };
    walk(walk, loop.bodyIndex, {}, {}, std::nullopt);
    if (invalid || paths.empty())
      continue;

    // Every write to the target object must belong to this complete producer.
    for (std::size_t block = 0; block < function.blocks().size(); ++block) {
      for (const auto &instruction : function.blocks()[block].instructions()) {
        if (instruction.opcode() != "store")
          continue;
        const auto root =
            pointerRoot(instruction.operands().back(), definitions);
        if (root == data && region.find(block) == region.end())
          invalid = true;
      }
    }
    if (invalid)
      continue;

    std::unordered_map<std::string, bool> memo;
    std::unordered_set<std::string> visiting;
    for (const auto &path : paths) {
      if (!initializerValueCloneable(function, definitions, cfg, loop, region,
                                     path.value, memo, visiting)) {
        invalid = true;
        break;
      }
      for (const auto &[condition, unused] : path.conditions) {
        (void)unused;
        if (!initializerValueCloneable(function, definitions, cfg, loop, region,
                                       condition, memo, visiting)) {
          invalid = true;
          break;
        }
      }
      if (invalid)
        break;
    }
    if (invalid)
      continue;
    return InitializationProducer{loop, std::move(paths), *defaultValue,
                                  std::move(region)};
  }
  return std::nullopt;
}

std::optional<Candidate> analyzeCaller(const Module &module,
                                       const CopyKernel &kernel) {
  const auto &callee = module.functions()[kernel.functionIndex];
  for (std::size_t functionIndex = 0; functionIndex < module.functions().size();
       ++functionIndex) {
    if (functionIndex == kernel.functionIndex)
      continue;
    const auto &function = module.functions()[functionIndex];
    const auto definitions = definitionsOf(function);
    const auto loops = simpleLoops(function, definitions);
    for (const auto &callLoop : loops) {
      if (callLoop.body != callLoop.backedge)
        continue;
      const auto afterCalls = reachableFrom(function, callLoop.headerIndex);
      const Instruction *call = nullptr;
      for (const auto &instruction :
           function.blocks()[callLoop.bodyIndex].instructions()) {
        if (instruction.opcode() == "call" &&
            instruction.operands().size() == 4 &&
            instruction.operands()[0] == "@" + callee.name()) {
          if (call != nullptr) {
            call = nullptr;
            break;
          }
          call = &instruction;
        }
      }
      if (call == nullptr || !valueUnused(function, *call))
        continue;
      bool callBodyHasOtherEffects = false;
      for (const auto &instruction :
           function.blocks()[callLoop.bodyIndex].instructions()) {
        if ((instruction.opcode() == "call" && &instruction != call) ||
            instruction.opcode() == "store") {
          callBodyHasOtherEffects = true;
        }
      }
      if (callBodyHasOtherEffects)
        continue;
      const std::string count = call->operands()[1];
      const std::string data = call->operands()[2];
      const auto rowDefinition = definitions.find(call->operands()[3]);
      if (rowDefinition == definitions.end() ||
          rowDefinition->second.instruction->opcode() != "load") {
        continue;
      }
      const auto rowAccess = pointerAccess(
          rowDefinition->second.instruction->operands().back(), definitions);
      if (!rowAccess.has_value() || rowAccess->index != callLoop.induction ||
          rowAccess->base == data) {
        continue;
      }
      const auto dataCapacity = globalCapacity(module, data);
      const auto parameterCapacity = globalCapacity(module, rowAccess->base);
      if (!dataCapacity.has_value() || !parameterCapacity.has_value()) {
        continue;
      }
      for (const auto &consumer : loops) {
        if (consumer.preheader != callLoop.exit ||
            consumer.bound != callLoop.bound ||
            consumer.body != consumer.backedge) {
          continue;
        }
        const Instruction *targetLoad = nullptr;
        std::string targetPointer;
        for (const auto &instruction :
             function.blocks()[consumer.bodyIndex].instructions()) {
          if (instruction.opcode() != "load")
            continue;
          const auto access =
              pointerAccess(instruction.operands().back(), definitions);
          if (!access.has_value() || access->base != data)
            continue;
          if (targetLoad != nullptr || access->index != consumer.induction) {
            targetLoad = nullptr;
            break;
          }
          targetLoad = &instruction;
          targetPointer = instruction.operands().back();
        }
        if (targetLoad == nullptr)
          continue;

        bool unsafe = false;
        for (std::size_t block = 0; block < function.blocks().size(); ++block) {
          for (const auto &instruction :
               function.blocks()[block].instructions()) {
            if (instruction.opcode() == "call") {
              for (std::size_t operand = 1;
                   operand < instruction.operands().size(); ++operand) {
                const auto root =
                    pointerRoot(instruction.operands()[operand], definitions);
                if (root == data && &instruction != call) {
                  unsafe = true;
                }
                if (root == rowAccess->base &&
                    afterCalls.find(block) != afterCalls.end()) {
                  unsafe = true;
                }
              }
            }
            if (instruction.opcode() != "load" &&
                instruction.opcode() != "store") {
              continue;
            }
            const auto root =
                pointerRoot(instruction.operands().back(), definitions);
            if (root == rowAccess->base) {
              if (instruction.opcode() == "store" &&
                  afterCalls.find(block) != afterCalls.end()) {
                unsafe = true;
              }
              continue;
            }
            if (root != data)
              continue;
            if (instruction.opcode() == "load" && &instruction != targetLoad) {
              unsafe = true;
            }
            if (instruction.opcode() == "store" &&
                afterCalls.find(block) != afterCalls.end()) {
              unsafe = true;
            }
          }
        }
        for (std::size_t other = 0; other < module.functions().size();
             ++other) {
          if (other == functionIndex || other == kernel.functionIndex) {
            continue;
          }
          const auto otherDefinitions =
              definitionsOf(module.functions()[other]);
          for (const auto &block : module.functions()[other].blocks()) {
            for (const auto &instruction : block.instructions()) {
              if (instruction.opcode() != "load" &&
                  instruction.opcode() != "store") {
                continue;
              }
              const auto root =
                  pointerRoot(instruction.operands().back(), otherDefinitions);
              if (root == data)
                unsafe = true;
            }
          }
        }
        if (unsafe)
          continue;
        const auto initializer = analyzeInitializationProducer(
            module, function, definitions, loops, callLoop, count, data);
        return Candidate{functionIndex,
                         callLoop,
                         consumer,
                         count,
                         callLoop.bound,
                         data,
                         rowAccess->base,
                         targetPointer,
                         *dataCapacity,
                         *parameterCapacity,
                         targetLoad->result(),
                         initializer};
      }
    }
  }
  return std::nullopt;
}

std::string blockName(const Function &function, const std::string &stem) {
  std::unordered_set<std::string> used;
  for (const auto &block : function.blocks())
    used.insert(block.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const auto name = stem + "." + std::to_string(suffix);
    if (used.find(name) == used.end())
      return name;
  }
}

std::string valueName(const Function &function, const std::string &stem) {
  std::unordered_set<std::string> used;
  for (const auto &parameter : function.parameters())
    used.insert(parameter.name);
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult())
        used.insert(instruction.result());
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const auto name = "%" + stem + "." + std::to_string(suffix);
    if (used.find(name) == used.end())
      return name;
  }
}

Instruction replaceOperand(const Instruction &instruction,
                           const std::string &from, const std::string &to) {
  auto operands = instruction.operands();
  for (auto &operand : operands) {
    if (operand == from)
      operand = to;
  }
  return Instruction{instruction.opcode(), std::move(operands),
                     instruction.result()};
}

std::string cloneInitializerValue(
    const Function &function, const DefinitionMap &definitions,
    const InitializationProducer &producer, const std::string &source,
    const std::string &resolvedIndex,
    std::unordered_map<std::string, std::string> &replacements,
    std::vector<Instruction> &output, std::size_t &counter) {
  long long literal = 0;
  if (detail::parseIntegerLiteral(source, literal))
    return source;
  if (source == producer.loop.induction)
    return resolvedIndex;
  const auto replaced = replacements.find(source);
  if (replaced != replacements.end())
    return replaced->second;
  const auto definition = definitions.find(source);
  if (definition == definitions.end() ||
      producer.region.find(definition->second.block) == producer.region.end() ||
      !isSafeInitializerOpcode(definition->second.instruction->opcode())) {
    return source;
  }
  std::vector<std::string> operands;
  operands.reserve(definition->second.instruction->operands().size());
  for (const auto &operand : definition->second.instruction->operands()) {
    operands.push_back(cloneInitializerValue(function, definitions, producer,
                                             operand, resolvedIndex,
                                             replacements, output, counter));
  }
  const auto result = valueName(function, "lazy.initializer.expression." +
                                              std::to_string(counter++));
  output.emplace_back(definition->second.instruction->opcode(),
                      std::move(operands), result);
  replacements.emplace(source, result);
  return result;
}

void applyCandidate(Module &module, const Candidate &candidate) {
  auto &function = module.functions()[candidate.functionIndex];
  const auto definitions = definitionsOf(function);
  const auto *originalConsumerPointer =
      definitions.at(candidate.consumerPointer).instruction;
  const std::string consumerPointerOpcode = originalConsumerPointer->opcode();
  const std::string consumerPointerBase =
      originalConsumerPointer->operands().front();
  const auto indices = blockIndices(function);
  const bool forwardInitializer =
      candidate.initializer.has_value() &&
      std::getenv("COMPILER_DISABLE_LAZY_INITIALIZER_VALUE_FORWARDING") ==
          nullptr;
  const auto validationEntry =
      blockName(function, "lazy.affine.validate.entry");
  const auto validationHeader =
      blockName(function, "lazy.affine.validate.header");
  const auto validationBody = blockName(function, "lazy.affine.validate.body");
  const auto validationLatch =
      blockName(function, "lazy.affine.validate.latch");
  const auto validationAccept =
      blockName(function, "lazy.affine.validate.accept");
  const auto validationFail = blockName(function, "lazy.affine.validate.fail");
  const auto passHeader = blockName(function, "lazy.affine.pass.header");
  const auto passPrepare = blockName(function, "lazy.affine.pass.prepare");
  const auto chainHeader = blockName(function, "lazy.affine.chain.header");
  const auto chainApply = blockName(function, "lazy.affine.chain.apply");
  const auto chainExit = blockName(function, "lazy.affine.chain.exit");
  const auto resume = blockName(function, "lazy.affine.resume");
  const auto fallbackValue =
      blockName(function, "lazy.affine.initializer.fallback");
  const auto lazyValue = blockName(function, "lazy.affine.initializer.value");

  const auto v = [&](const std::string &stem) {
    return valueName(function, stem);
  };
  const auto modeSlot = v("lazy.affine.mode");
  const auto safeCount = v("lazy.affine.safe.count");
  const auto safeLength = v("lazy.affine.safe.length");
  const auto countCapacity = v("lazy.affine.count.capacity");
  const auto lengthCapacity = v("lazy.affine.length.capacity");
  const auto safePair = v("lazy.affine.safe.pair");
  const auto safeCapacity = v("lazy.affine.safe.capacity");
  const auto validationSafe = v("lazy.affine.validation.safe");
  const auto validationIndex = v("lazy.affine.validation.index");
  const auto validationNext = v("lazy.affine.validation.next");
  const auto validationMore = v("lazy.affine.validation.more");
  const auto validationPointer = v("lazy.affine.validation.pointer");
  const auto validationRow = v("lazy.affine.validation.row");
  const auto rowPositive = v("lazy.affine.row.positive");
  const auto rowWithin = v("lazy.affine.row.within");
  const auto rowSafe = v("lazy.affine.row.safe");
  const auto mode = v("lazy.affine.mode.value");
  const auto useLazy = v("lazy.affine.use");
  const auto passIndex = v("lazy.affine.pass.index");
  const auto passCounter = v("lazy.affine.pass.counter");
  const auto passMore = v("lazy.affine.pass.more");
  const auto previousPass = v("lazy.affine.pass.previous");
  const auto rowPointer = v("lazy.affine.row.pointer");
  const auto row = v("lazy.affine.row");
  const auto column = v("lazy.affine.column");
  const auto chainIndex = v("lazy.affine.chain.index");
  const auto cutoff = v("lazy.affine.chain.cutoff");
  const auto destinationRow = v("lazy.affine.destination.row");
  const auto destinationColumn = v("lazy.affine.destination.column");
  const auto sourceProduct = v("lazy.affine.source.product");
  const auto sourceIndex = v("lazy.affine.source.index");
  const auto rowValid = v("lazy.affine.destination.row.valid");
  const auto triangular = v("lazy.affine.triangular");
  const auto earlier = v("lazy.affine.earlier.writer");
  const auto destinationValid = v("lazy.affine.destination.valid");
  const auto follow = v("lazy.affine.follow");
  const auto resolvedIndex = v("lazy.affine.resolved.index");
  const auto fallbackPointer = v("lazy.initializer.fallback.pointer");
  const auto fallbackLoaded = v("lazy.initializer.fallback.loaded");
  const auto initializedNonnegative = v("lazy.initializer.nonnegative");
  const auto initializedBelowBound = v("lazy.initializer.below.bound");
  const auto initializedInRange = v("lazy.initializer.in.range");
  const auto initializedValue = v("lazy.initializer.value");
  const auto materializedValue = v("lazy.initializer.materialized");

  auto &entry = function.blocks().front().instructions();
  entry.insert(entry.end() - 1, Instruction{"alloca", {"int"}, modeSlot});

  if (forwardInitializer) {
    const auto &initializer = *candidate.initializer;
    auto &initializationPreheader =
        function.blocks()[indices.at(initializer.loop.preheader)]
            .instructions();
    initializationPreheader.back() =
        replaceOperand(initializationPreheader.back(), initializer.loop.header,
                       validationEntry);
    auto &initializationHeader =
        function.blocks()[indices.at(initializer.loop.header)].instructions();
    for (auto &instruction : initializationHeader) {
      if (instruction.opcode() == "phi" &&
          instruction.result() == initializer.loop.induction) {
        instruction = replaceOperand(instruction, initializer.loop.preheader,
                                     validationFail);
      }
    }
  } else {
    auto &oldPreheader =
        function.blocks()[indices.at(candidate.calls.preheader)].instructions();
    oldPreheader.back() = replaceOperand(
        oldPreheader.back(), candidate.calls.header, validationEntry);

    auto &callHeader =
        function.blocks()[indices.at(candidate.calls.header)].instructions();
    for (auto &instruction : callHeader) {
      if (instruction.opcode() == "phi" &&
          instruction.result() == candidate.calls.induction) {
        instruction = replaceOperand(instruction, candidate.calls.preheader,
                                     validationFail);
      }
    }
  }

  auto &consumerHeader =
      function.blocks()[indices.at(candidate.consumer.header)].instructions();
  for (auto &instruction : consumerHeader) {
    if (instruction.opcode() == "phi" &&
        instruction.result() == candidate.consumer.induction) {
      instruction =
          replaceOperand(instruction, candidate.consumer.body, resume);
    }
  }

  auto &oldBody =
      function.blocks()[indices.at(candidate.consumer.body)].instructions();
  // Keep the original call loop as a runtime fallback.  On the lazy path,
  // resolve each demanded array index backwards through the call sequence.
  // Within one copy pass, sourceIndex is also the original lexicographic
  // operation number.  Chasing only writers with sourceIndex < cutoff
  // therefore reproduces values written earlier in that same pass.
  std::vector<Instruction> body;
  if (forwardInitializer) {
    body.push_back(Instruction{
        "phi",
        {"int", fallbackLoaded, fallbackValue, initializedValue, lazyValue},
        materializedValue});
    for (const auto &instruction : oldBody) {
      if ((instruction.result() == candidate.consumerPointer &&
           isGep(instruction)) ||
          instruction.result() == candidate.targetLoad) {
        continue;
      }
      body.push_back(
          replaceOperand(instruction, candidate.targetLoad, materializedValue));
    }
  } else {
    body.push_back(Instruction{"phi",
                               {"int", candidate.consumer.induction,
                                candidate.consumer.body, passIndex, passHeader},
                               resolvedIndex});
    for (const auto &instruction : oldBody) {
      if (instruction.result() == candidate.consumerPointer &&
          isGep(instruction)) {
        body.push_back(Instruction{instruction.opcode(),
                                   {instruction.operands()[0], resolvedIndex},
                                   instruction.result()});
      } else {
        body.push_back(instruction);
      }
    }
  }
  oldBody.clear();
  oldBody.push_back(Instruction{"load", {"int", modeSlot}, mode});
  oldBody.push_back(Instruction{"icmp.ne", {mode, "0"}, useLazy});
  oldBody.push_back(Instruction{
      "br",
      {useLazy, passHeader, forwardInitializer ? fallbackValue : resume}});

  BasicBlock validateEntry{validationEntry};
  validateEntry.addInstruction(
      Instruction{"icmp.sgt", {candidate.count, "0"}, safeCount});
  validateEntry.addInstruction(
      Instruction{"icmp.sge", {candidate.length, "0"}, safeLength});
  validateEntry.addInstruction(
      Instruction{"icmp.sle",
                  {candidate.count, std::to_string(candidate.dataCapacity)},
                  countCapacity});
  validateEntry.addInstruction(Instruction{
      "icmp.sle",
      {candidate.length, std::to_string(candidate.parameterCapacity)},
      lengthCapacity});
  validateEntry.addInstruction(
      Instruction{"and", {safeCount, safeLength}, safePair});
  validateEntry.addInstruction(
      Instruction{"and", {countCapacity, lengthCapacity}, safeCapacity});
  validateEntry.addInstruction(
      Instruction{"and", {safePair, safeCapacity}, validationSafe});
  validateEntry.addInstruction(
      Instruction{"br", {validationSafe, validationHeader, validationFail}});

  BasicBlock validateHeaderBlock{validationHeader};
  validateHeaderBlock.addInstruction(Instruction{
      "phi",
      {"int", "0", validationEntry, validationNext, validationLatch},
      validationIndex});
  validateHeaderBlock.addInstruction(Instruction{
      "icmp.slt", {validationIndex, candidate.length}, validationMore});
  validateHeaderBlock.addInstruction(
      Instruction{"br", {validationMore, validationBody, validationAccept}});

  BasicBlock validateBodyBlock{validationBody};
  validateBodyBlock.addInstruction(
      Instruction{"getelementptr",
                  {candidate.parameters, validationIndex},
                  validationPointer});
  validateBodyBlock.addInstruction(
      Instruction{"load", {"int", validationPointer}, validationRow});
  validateBodyBlock.addInstruction(
      Instruction{"icmp.sgt", {validationRow, "0"}, rowPositive});
  validateBodyBlock.addInstruction(
      Instruction{"icmp.sle", {validationRow, candidate.count}, rowWithin});
  validateBodyBlock.addInstruction(
      Instruction{"and", {rowPositive, rowWithin}, rowSafe});
  validateBodyBlock.addInstruction(
      Instruction{"br", {rowSafe, validationLatch, validationFail}});

  BasicBlock validateLatchBlock{validationLatch};
  validateLatchBlock.addInstruction(
      Instruction{"add", {validationIndex, "1"}, validationNext});
  validateLatchBlock.addInstruction(Instruction{"br", {validationHeader}});

  BasicBlock validateAcceptBlock{validationAccept};
  validateAcceptBlock.addInstruction(
      Instruction{"store", {"int", "1", modeSlot}});
  validateAcceptBlock.addInstruction(Instruction{"br", {candidate.calls.exit}});

  BasicBlock validateFailBlock{validationFail};
  validateFailBlock.addInstruction(
      Instruction{"store", {"int", "0", modeSlot}});
  validateFailBlock.addInstruction(
      Instruction{"br",
                  {forwardInitializer ? candidate.initializer->loop.header
                                      : candidate.calls.header}});

  BasicBlock passHeaderBlock{passHeader};
  passHeaderBlock.addInstruction(
      Instruction{"phi",
                  {"int", candidate.consumer.induction, candidate.consumer.body,
                   chainIndex, chainExit},
                  passIndex});
  passHeaderBlock.addInstruction(
      Instruction{"phi",
                  {"int", candidate.length, candidate.consumer.body,
                   previousPass, chainExit},
                  passCounter});
  passHeaderBlock.addInstruction(
      Instruction{"icmp.sgt", {passCounter, "0"}, passMore});
  passHeaderBlock.addInstruction(Instruction{
      "br", {passMore, passPrepare, forwardInitializer ? lazyValue : resume}});

  BasicBlock passPrepareBlock{passPrepare};
  passPrepareBlock.addInstruction(
      Instruction{"sub", {passCounter, "1"}, previousPass});
  passPrepareBlock.addInstruction(Instruction{
      "getelementptr", {candidate.parameters, previousPass}, rowPointer});
  passPrepareBlock.addInstruction(
      Instruction{"load", {"int", rowPointer}, row});
  passPrepareBlock.addInstruction(
      Instruction{"sdiv", {candidate.count, row}, column});
  passPrepareBlock.addInstruction(Instruction{"br", {chainHeader}});

  BasicBlock chainHeaderBlock{chainHeader};
  chainHeaderBlock.addInstruction(
      Instruction{"phi",
                  {"int", passIndex, passPrepare, sourceIndex, chainApply},
                  chainIndex});
  chainHeaderBlock.addInstruction(Instruction{
      "phi",
      {"int", candidate.count, passPrepare, sourceIndex, chainApply},
      cutoff});
  chainHeaderBlock.addInstruction(
      Instruction{"sdiv", {chainIndex, column}, destinationRow});
  chainHeaderBlock.addInstruction(
      Instruction{"srem", {chainIndex, column}, destinationColumn});
  chainHeaderBlock.addInstruction(
      Instruction{"mul", {destinationColumn, row}, sourceProduct});
  chainHeaderBlock.addInstruction(
      Instruction{"add", {sourceProduct, destinationRow}, sourceIndex});
  chainHeaderBlock.addInstruction(
      Instruction{"icmp.slt", {destinationRow, row}, rowValid});
  chainHeaderBlock.addInstruction(
      Instruction{"icmp.sle", {destinationRow, destinationColumn}, triangular});
  chainHeaderBlock.addInstruction(
      Instruction{"icmp.slt", {sourceIndex, cutoff}, earlier});
  chainHeaderBlock.addInstruction(
      Instruction{"and", {rowValid, triangular}, destinationValid});
  chainHeaderBlock.addInstruction(
      Instruction{"and", {destinationValid, earlier}, follow});
  chainHeaderBlock.addInstruction(
      Instruction{"br", {follow, chainApply, chainExit}});

  BasicBlock chainApplyBlock{chainApply};
  chainApplyBlock.addInstruction(Instruction{"br", {chainHeader}});

  BasicBlock chainExitBlock{chainExit};
  chainExitBlock.addInstruction(Instruction{"br", {passHeader}});

  BasicBlock resumeBlock{resume};
  for (auto &instruction : body) {
    resumeBlock.addInstruction(std::move(instruction));
  }

  BasicBlock fallbackValueBlock{fallbackValue};
  BasicBlock lazyValueBlock{lazyValue};
  if (forwardInitializer) {
    fallbackValueBlock.addInstruction(
        Instruction{consumerPointerOpcode,
                    {consumerPointerBase, candidate.consumer.induction},
                    fallbackPointer});
    fallbackValueBlock.addInstruction(
        Instruction{"load", {"int", fallbackPointer}, fallbackLoaded});
    fallbackValueBlock.addInstruction(Instruction{"br", {resume}});

    const auto &initializer = *candidate.initializer;
    std::unordered_map<std::string, std::string> replacements;
    std::vector<Instruction> generated;
    std::size_t expressionCounter = 0;
    const auto clone = [&](const std::string &value) {
      return cloneInitializerValue(function, definitions, initializer, value,
                                   passIndex, replacements, generated,
                                   expressionCounter);
    };
    std::vector<std::string> pathValues;
    pathValues.reserve(initializer.paths.size());
    for (const auto &path : initializer.paths) {
      pathValues.push_back(clone(path.value));
    }
    std::string generatedValue = pathValues.back();
    for (std::size_t remaining = initializer.paths.size() - 1; remaining > 0;
         --remaining) {
      const auto pathIndex = remaining - 1;
      std::string predicate;
      for (std::size_t conditionIndex = 0;
           conditionIndex < initializer.paths[pathIndex].conditions.size();
           ++conditionIndex) {
        const auto &[condition, positive] =
            initializer.paths[pathIndex].conditions[conditionIndex];
        std::string term = clone(condition);
        if (!positive) {
          const auto inverted =
              v("lazy.initializer.path.not." + std::to_string(pathIndex) + "." +
                std::to_string(conditionIndex));
          generated.emplace_back("xor", std::vector<std::string>{term, "1"},
                                 inverted);
          term = inverted;
        }
        if (predicate.empty()) {
          predicate = term;
        } else {
          const auto conjunction =
              v("lazy.initializer.path.and." + std::to_string(pathIndex) + "." +
                std::to_string(conditionIndex));
          generated.emplace_back(
              "and", std::vector<std::string>{predicate, term}, conjunction);
          predicate = conjunction;
        }
      }
      if (predicate.empty()) {
        generatedValue = pathValues[pathIndex];
      } else {
        const auto selected =
            v("lazy.initializer.path.select." + std::to_string(pathIndex));
        generated.emplace_back("select",
                               std::vector<std::string>{predicate,
                                                        pathValues[pathIndex],
                                                        generatedValue},
                               selected);
        generatedValue = selected;
      }
    }
    for (auto &instruction : generated) {
      lazyValueBlock.addInstruction(std::move(instruction));
    }
    lazyValueBlock.addInstruction(
        Instruction{"icmp.sge", {passIndex, "0"}, initializedNonnegative});
    lazyValueBlock.addInstruction(Instruction{
        "icmp.slt", {passIndex, candidate.count}, initializedBelowBound});
    lazyValueBlock.addInstruction(
        Instruction{"and",
                    {initializedNonnegative, initializedBelowBound},
                    initializedInRange});
    lazyValueBlock.addInstruction(Instruction{
        "select",
        {initializedInRange, generatedValue, initializer.defaultValue},
        initializedValue});
    lazyValueBlock.addInstruction(Instruction{"br", {resume}});
  }

  function.addBlock(std::move(validateEntry));
  function.addBlock(std::move(validateHeaderBlock));
  function.addBlock(std::move(validateBodyBlock));
  function.addBlock(std::move(validateLatchBlock));
  function.addBlock(std::move(validateAcceptBlock));
  function.addBlock(std::move(validateFailBlock));
  function.addBlock(std::move(passHeaderBlock));
  function.addBlock(std::move(passPrepareBlock));
  function.addBlock(std::move(chainHeaderBlock));
  function.addBlock(std::move(chainApplyBlock));
  function.addBlock(std::move(chainExitBlock));
  if (forwardInitializer) {
    function.addBlock(std::move(fallbackValueBlock));
    function.addBlock(std::move(lazyValueBlock));
  }
  function.addBlock(std::move(resumeBlock));
}

} // namespace

void LazyAffineCopyResolutionPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_LAZY_AFFINE_COPY_RESOLUTION") != nullptr) {
    return;
  }
  std::size_t transformed = 0;
  std::size_t initializerForwarded = 0;
  for (std::size_t functionIndex = 0; functionIndex < module.functions().size();
       ++functionIndex) {
    const auto kernel =
        analyzeKernel(module.functions()[functionIndex], functionIndex);
    if (!kernel.has_value())
      continue;
    const auto candidate = analyzeCaller(module, *kernel);
    if (!candidate.has_value())
      continue;
    if (candidate->initializer.has_value() &&
        std::getenv("COMPILER_DISABLE_LAZY_INITIALIZER_VALUE_FORWARDING") ==
            nullptr) {
      ++initializerForwarded;
    }
    applyCandidate(module, *candidate);
    ++transformed;
    break;
  }
  if (std::getenv("COMPILER_LAZY_AFFINE_COPY_STATS") != nullptr) {
    std::cerr << "[lazy-affine-copy] transformed=" << transformed
              << " initializer_forwarded=" << initializerForwarded << '\n';
  }
}

} // namespace compiler::midend::pass
