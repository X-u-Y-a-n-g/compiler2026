#include "../../../include/compiler/midend/pass/StoreProducerReductionFusionPass.h"

#include <algorithm>
#include <cstdlib>
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
  std::vector<std::vector<std::string>> successors;
  std::vector<std::vector<std::string>> predecessors;
};

using DefinitionMap =
    std::unordered_map<std::string, const Instruction*>;

struct ReductionScan {
  std::string squareBlock;
  std::string scanHeader;
  std::string producerExit;
  std::string scaleExit;
  std::string reductionSlot;
  std::string matrixRoot;
  std::string count;
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
  cfg.successors.resize(function.blocks().size());
  cfg.predecessors.resize(function.blocks().size());
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    cfg.index.emplace(function.blocks()[index].name(), index);
  }
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    for (const auto& target : branchTargets(function.blocks()[index])) {
      const auto found = cfg.index.find(target);
      if (found == cfg.index.end()) continue;
      cfg.successors[index].push_back(target);
      cfg.predecessors[found->second].push_back(
          function.blocks()[index].name());
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
    if (block.name() == name) return &block;
  }
  return nullptr;
}

std::unordered_set<std::string> reachable(
    const CFG& cfg, const std::string& start, bool reverse = false) {
  std::unordered_set<std::string> visited;
  std::vector<std::string> work{start};
  while (!work.empty()) {
    const std::string current = std::move(work.back());
    work.pop_back();
    if (!visited.insert(current).second) continue;
    const auto found = cfg.index.find(current);
    if (found == cfg.index.end()) continue;
    const auto& edges =
        reverse ? cfg.predecessors[found->second] : cfg.successors[found->second];
    work.insert(work.end(), edges.begin(), edges.end());
  }
  return visited;
}

std::unordered_set<std::string> stronglyConnectedRegion(
    const CFG& cfg, const std::string& seed) {
  const auto forward = reachable(cfg, seed);
  const auto backward = reachable(cfg, seed, true);
  std::unordered_set<std::string> region;
  for (const auto& block : forward) {
    if (backward.find(block) != backward.end()) region.insert(block);
  }
  return region;
}

std::string pointerRoot(const std::string& value,
                        const DefinitionMap& definitions) {
  std::string current = value;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    const auto found = definitions.find(current);
    if (found == definitions.end()) return current;
    const auto* instruction = found->second;
    if ((instruction->opcode() != "getelementptr" &&
         instruction->opcode() != "gep.linear") ||
        instruction->operands().empty()) {
      return current;
    }
    current = instruction->operands()[0];
  }
  return {};
}

bool isLoadFrom(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "load" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         instruction.operands()[0] == "int" &&
         instruction.operands()[1] == slot;
}

bool isStoreTo(const Instruction& instruction, const std::string& slot) {
  return instruction.opcode() == "store" &&
         instruction.operands().size() == 3 &&
         instruction.operands()[0] == "int" &&
         instruction.operands()[2] == slot;
}

std::optional<std::pair<std::string, std::string>> findScale(
    const Function& function, const std::string& reductionSlot) {
  for (const auto& block : function.blocks()) {
    for (const auto& load : block.instructions()) {
      if (!isLoadFrom(load, reductionSlot)) continue;
      for (const auto& multiply : block.instructions()) {
        if (multiply.opcode() != "mul" || !multiply.hasResult() ||
            multiply.operands().size() != 2) {
          continue;
        }
        std::string count;
        if (multiply.operands()[0] == load.result()) {
          count = multiply.operands()[1];
        } else if (multiply.operands()[1] == load.result()) {
          count = multiply.operands()[0];
        } else {
          continue;
        }
        const bool stored = std::any_of(
            block.instructions().begin(), block.instructions().end(),
            [&](const Instruction& instruction) {
              return isStoreTo(instruction, reductionSlot) &&
                     instruction.operands()[1] == multiply.result();
            });
        if (stored) return std::pair{block.name(), count};
      }
    }
  }
  return std::nullopt;
}

std::optional<ReductionScan> findReductionScan(const Function& function,
                                               const CFG& cfg,
                                               const DefinitionMap& definitions) {
  for (const auto& squareBlock : function.blocks()) {
    for (const auto& square : squareBlock.instructions()) {
      if (square.opcode() != "mul" || square.operands().size() != 2 ||
          square.operands()[0] != square.operands()[1]) {
        continue;
      }
      const auto valueFound = definitions.find(square.operands()[0]);
      if (valueFound == definitions.end() ||
          valueFound->second->opcode() != "load" ||
          valueFound->second->operands().size() != 2 ||
          valueFound->second->operands()[0] != "int") {
        continue;
      }
      const std::string matrixRoot =
          pointerRoot(valueFound->second->operands()[1], definitions);
      if (matrixRoot.empty() || matrixRoot.front() != '@') continue;

      for (const auto& add : squareBlock.instructions()) {
        if (add.opcode() != "add" || !add.hasResult() ||
            add.operands().size() != 2) {
          continue;
        }
        std::string reductionValue;
        if (add.operands()[0] == square.result()) {
          reductionValue = add.operands()[1];
        } else if (add.operands()[1] == square.result()) {
          reductionValue = add.operands()[0];
        } else {
          continue;
        }
        const auto reductionLoad = definitions.find(reductionValue);
        if (reductionLoad == definitions.end() ||
            reductionLoad->second->opcode() != "load" ||
            reductionLoad->second->operands().size() != 2 ||
            reductionLoad->second->operands()[0] != "int") {
          continue;
        }
        const std::string reductionSlot =
            reductionLoad->second->operands()[1];
        const bool stored = std::any_of(
            squareBlock.instructions().begin(),
            squareBlock.instructions().end(),
            [&](const Instruction& instruction) {
              return isStoreTo(instruction, reductionSlot) &&
                     instruction.operands()[1] == add.result();
            });
        if (!stored) continue;

        const auto scale = findScale(function, reductionSlot);
        if (!scale.has_value()) continue;
        const auto scanRegion =
            stronglyConnectedRegion(cfg, squareBlock.name());
        if (scanRegion.size() < 2) continue;

        std::vector<std::pair<std::string, std::string>> entries;
        for (const auto& name : scanRegion) {
          const auto index = cfg.index.at(name);
          for (const auto& predecessor : cfg.predecessors[index]) {
            if (scanRegion.find(predecessor) == scanRegion.end()) {
              entries.emplace_back(predecessor, name);
            }
          }
        }
        if (entries.size() != 1) continue;
        const auto* header = findBlock(function, cfg, entries[0].second);
        if (header == nullptr || header->terminator() == nullptr ||
            header->terminator()->operands().size() != 3 ||
            header->terminator()->operands()[2] != scale->first) {
          continue;
        }
        bool firstIterationGuard = false;
        for (const auto& instruction : header->instructions()) {
          if (instruction.opcode() == "icmp.eq" &&
              instruction.operands().size() == 2 &&
              (detail::isZero(instruction.operands()[0]) ||
               detail::isZero(instruction.operands()[1]))) {
            firstIterationGuard = true;
          }
        }
        if (!firstIterationGuard) continue;
        return ReductionScan{squareBlock.name(), entries[0].second,
                             entries[0].first, scale->first, reductionSlot,
                             matrixRoot, scale->second};
      }
    }
  }
  return std::nullopt;
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = "%" + stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) names.insert(block.name());
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.find(candidate) == names.end()) return candidate;
  }
}

bool rewriteBranchTarget(BasicBlock& block, const std::string& oldTarget,
                         const std::string& newTarget) {
  if (block.instructions().empty()) return false;
  auto& terminator = block.instructions().back();
  if (terminator.opcode() != "br") return false;
  auto operands = terminator.operands();
  bool changed = false;
  const std::size_t begin = operands.size() == 1 ? 0 : 1;
  for (std::size_t index = begin; index < operands.size(); ++index) {
    if (operands[index] == oldTarget) {
      operands[index] = newTarget;
      changed = true;
    }
  }
  if (changed) terminator = Instruction{"br", std::move(operands)};
  return changed;
}

bool transform(Function& function, const ReductionScan& scan) {
  const CFG cfg = buildCFG(function);
  const DefinitionMap definitions = buildDefinitions(function);

  std::optional<std::string> seed;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "aarch64.neon.store.acc.i32x32" &&
          instruction.operands().size() == 1 &&
          pointerRoot(instruction.operands()[0], definitions) ==
              scan.matrixRoot) {
        seed = block.name();
        break;
      }
    }
    if (seed.has_value()) break;
  }
  if (!seed.has_value()) return false;

  const auto producerRegion = stronglyConnectedRegion(cfg, *seed);
  if (producerRegion.empty() ||
      producerRegion.find(scan.producerExit) != producerRegion.end()) {
    return false;
  }

  std::vector<std::pair<std::string, std::string>> entries;
  std::unordered_set<std::string> exits;
  for (const auto& name : producerRegion) {
    const auto index = cfg.index.at(name);
    for (const auto& predecessor : cfg.predecessors[index]) {
      if (producerRegion.find(predecessor) == producerRegion.end()) {
        entries.emplace_back(predecessor, name);
      }
    }
    for (const auto& successor : cfg.successors[index]) {
      if (producerRegion.find(successor) == producerRegion.end()) {
        exits.insert(successor);
      }
    }
  }
  if (entries.size() != 1 || exits.size() != 1 ||
      *exits.begin() != scan.producerExit) {
    return false;
  }

  std::size_t vectorStores = 0;
  std::unordered_map<std::string, std::unordered_set<std::size_t>>
      scalarStores;
  std::unordered_set<std::string> localSlots;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        localSlots.insert(instruction.result());
      }
    }
  }
  for (const auto& name : producerRegion) {
    const auto* block = findBlock(function, cfg, name);
    for (std::size_t index = 0; index < block->instructions().size();
         ++index) {
      const auto& instruction = block->instructions()[index];
      if (instruction.opcode() == "call") return false;
      const auto& opcode = instruction.opcode();
      if (opcode.rfind("aarch64.neon.", 0) == 0 &&
          opcode != "aarch64.neon.zero.acc.i32x32" &&
          opcode != "aarch64.neon.splat.acc.i32x32" &&
          opcode != "aarch64.neon.dup.acc.i32x32" &&
          opcode != "aarch64.neon.mla.acc.i32x32" &&
          opcode != "aarch64.neon.store.acc.i32x32" &&
          opcode.rfind("aarch64.neon.ordered.panel.i32x16.", 0) != 0 &&
          opcode.rfind("aarch64.neon.ordered.panel4.i32x16.lazy.", 0) != 0) {
        return false;
      }
      if (instruction.opcode() == "aarch64.neon.store.acc.i32x32") {
        if (instruction.operands().size() != 1 ||
            pointerRoot(instruction.operands()[0], definitions) !=
                scan.matrixRoot) {
          return false;
        }
        ++vectorStores;
      } else if (instruction.opcode() ==
                 "aarch64.neon.ordered.panel.i32x16.store") {
        if (instruction.operands().size() != 2 ||
            pointerRoot(instruction.operands()[0], definitions) !=
                scan.matrixRoot ||
            pointerRoot(instruction.operands()[1], definitions) !=
                scan.matrixRoot) {
          return false;
        }
        ++vectorStores;
      } else if (instruction.opcode() ==
                 "aarch64.neon.ordered.panel4.i32x16.lazy.store") {
        if (instruction.operands().size() != 4) return false;
        for (const auto& operand : instruction.operands()) {
          if (pointerRoot(operand, definitions) != scan.matrixRoot) {
            return false;
          }
        }
        ++vectorStores;
      } else if (instruction.opcode() == "store") {
        if (instruction.operands().size() != 3 ||
            instruction.operands()[0] != "int") {
          return false;
        }
        const std::string root =
            pointerRoot(instruction.operands()[2], definitions);
        if (root == scan.matrixRoot) {
          scalarStores[name].insert(index);
        } else if (localSlots.find(root) == localSlots.end()) {
          return false;
        }
      }
    }
  }
  if (vectorStores == 0) return false;

  auto* entryPredecessor = findBlock(function, entries[0].first);
  auto* entryHeader = findBlock(function, entries[0].second);
  auto* producerExit = findBlock(function, scan.producerExit);
  if (entryPredecessor == nullptr || entryHeader == nullptr ||
      producerExit == nullptr) {
    return false;
  }

  const std::string initBlockName =
      freshBlockName(function, "store.reduce.init");
  if (!rewriteBranchTarget(*entryPredecessor, entries[0].second,
                           initBlockName)) {
    return false;
  }
  for (auto& instruction : entryHeader->instructions()) {
    if (instruction.opcode() != "phi") break;
    auto operands = instruction.operands();
    bool changed = false;
    for (std::size_t index = 2; index < operands.size(); index += 2) {
      if (operands[index] == entries[0].first) {
        operands[index] = initBlockName;
        changed = true;
      }
    }
    if (changed) {
      instruction =
          Instruction{"phi", std::move(operands), instruction.result()};
    }
  }

  for (auto& block : function.blocks()) {
    if (producerRegion.find(block.name()) == producerRegion.end()) continue;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size() + 2);
    for (std::size_t index = 0; index < block.instructions().size();
         ++index) {
      const auto& instruction = block.instructions()[index];
      rewritten.push_back(instruction);
      if (instruction.opcode() == "aarch64.neon.store.acc.i32x32") {
        rewritten.emplace_back(
            "aarch64.neon.square.sum.i32.accumulate.i32x32");
      } else if (instruction.opcode() ==
                 "aarch64.neon.ordered.panel.i32x16.store") {
        rewritten.emplace_back(
            "aarch64.neon.square.sum.i32.accumulate.panel.i32x16x2");
      } else if (instruction.opcode() ==
                 "aarch64.neon.ordered.panel4.i32x16.lazy.store") {
        rewritten.emplace_back(
            "aarch64.neon.square.sum.i32.accumulate.panel4.i32x16");
      } else if (scalarStores[block.name()].find(index) !=
                 scalarStores[block.name()].end()) {
        rewritten.emplace_back(
            "aarch64.neon.square.sum.i32.accumulate.scalar",
            std::vector<std::string>{instruction.operands()[1]});
      }
    }
    block.instructions() = std::move(rewritten);
  }

  auto exitInstructions = producerExit->instructions();
  std::vector<Instruction> rewrittenExit;
  rewrittenExit.reserve(exitInstructions.size() + 4);
  for (const auto& instruction : exitInstructions) {
    if (isStoreTo(instruction, scan.reductionSlot) &&
        detail::isZero(instruction.operands()[1])) {
      continue;
    }
    if (instruction.isTerminator()) continue;
    rewrittenExit.push_back(instruction);
  }
  const std::string sum =
      freshValueName(function, "store.reduce.square.sum");
  const std::string hasIterations =
      freshValueName(function, "store.reduce.has.iterations");
  const std::string selected =
      freshValueName(function, "store.reduce.selected.sum");
  rewrittenExit.emplace_back("aarch64.neon.square.sum.i32.finish",
                             std::vector<std::string>{}, sum);
  rewrittenExit.emplace_back("icmp.sgt",
                             std::vector<std::string>{scan.count, "0"},
                             hasIterations);
  rewrittenExit.emplace_back(
      "select", std::vector<std::string>{hasIterations, sum, "0"}, selected);
  rewrittenExit.emplace_back(
      "store",
      std::vector<std::string>{"int", selected, scan.reductionSlot});
  rewrittenExit.emplace_back("br", std::vector<std::string>{scan.scaleExit});
  producerExit->instructions() = std::move(rewrittenExit);

  BasicBlock init{initBlockName};
  init.addInstruction(Instruction{"aarch64.neon.square.sum.i32.init"});
  init.addInstruction(Instruction{"br", {entries[0].second}});
  function.addBlock(std::move(init));
  return true;
}

bool fuseOne(Function& function) {
  const CFG cfg = buildCFG(function);
  const DefinitionMap definitions = buildDefinitions(function);
  const auto scan = findReductionScan(function, cfg, definitions);
  return scan.has_value() && transform(function, *scan);
}

}  // namespace

void StoreProducerReductionFusionPass::run(
    compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_STORE_PRODUCER_REDUCTION_FUSION") !=
      nullptr) {
    return;
  }
  for (auto& function : module.functions()) {
    fuseOne(function);
  }
}

}  // namespace compiler::midend::pass
