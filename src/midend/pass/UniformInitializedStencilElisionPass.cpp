#include "../../../include/compiler/midend/pass/UniformInitializedStencilElisionPass.h"

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

struct Def {
  const Instruction *instruction = nullptr;
  std::size_t block = 0;
};

using Defs = std::unordered_map<std::string, Def>;

struct LoopShape {
  std::size_t header = 0;
  std::size_t latch = 0;
  std::string induction;
  std::string bound;
};

struct Candidate {
  std::size_t outerHeader = 0;
  std::size_t middleHeader = 0;
  std::size_t innerHeader = 0;
  std::size_t middleLatch = 0;
  std::string array;
  std::string seed;
  std::string outer;
  std::string middle;
  std::string inner;
  std::string bound;
  std::string stencilOuter;
  std::string stencilMiddle;
  std::string stencilInner;
};

bool isGep(const Instruction &instruction) {
  return instruction.opcode() == "getelementptr" ||
         instruction.opcode() == "getelementptr.linear" ||
         instruction.opcode() == "gep" || instruction.opcode() == "gep.linear";
}

bool isRankThreeIntArray(const std::string &type) {
  std::size_t dimensions = 0;
  std::size_t position = 0;
  while ((position = type.find('[', position)) != std::string::npos) {
    const auto close = type.find(']', position + 1);
    if (close == std::string::npos)
      return false;
    ++dimensions;
    position = close + 1;
  }
  if (dimensions != 3)
    return false;
  const auto leaf = type.substr(0, type.find('['));
  return leaf == "int" || leaf == "i32";
}

std::vector<std::string> targets(const BasicBlock &block) {
  const auto *terminator = block.terminator();
  if (terminator == nullptr || terminator->opcode() != "br")
    return {};
  if (terminator->operands().size() == 1)
    return {terminator->operands()[0]};
  if (terminator->operands().size() == 3)
    return {terminator->operands()[1], terminator->operands()[2]};
  return {};
}

std::unordered_map<std::string, std::size_t>
blockIndices(const Function &function) {
  std::unordered_map<std::string, std::size_t> result;
  for (std::size_t index = 0; index < function.blocks().size(); ++index)
    result.emplace(function.blocks()[index].name(), index);
  return result;
}

std::vector<std::vector<std::size_t>> predecessors(const Function &function) {
  const auto indices = blockIndices(function);
  std::vector<std::vector<std::size_t>> result(function.blocks().size());
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    for (const auto &target : targets(function.blocks()[block])) {
      const auto found = indices.find(target);
      if (found != indices.end())
        result[found->second].push_back(block);
    }
  }
  return result;
}

Defs definitions(const Function &function) {
  Defs result;
  for (std::size_t block = 0; block < function.blocks().size(); ++block)
    for (const auto &instruction : function.blocks()[block].instructions())
      if (instruction.hasResult())
        result.emplace(instruction.result(), Def{&instruction, block});
  return result;
}

bool isAddOne(const Instruction &instruction, const std::string &value) {
  return instruction.opcode() == "add" && instruction.hasResult() &&
         instruction.operands().size() == 2 &&
         ((instruction.operands()[0] == value &&
           instruction.operands()[1] == "1") ||
          (instruction.operands()[1] == value &&
           instruction.operands()[0] == "1"));
}

std::optional<LoopShape>
findLoop(const Function &function, std::size_t header,
         const std::vector<std::vector<std::size_t>> &preds) {
  const bool stats =
      std::getenv("COMPILER_UNIFORM_INITIALIZED_STENCIL_STATS") != nullptr;
  const auto &instructions = function.blocks()[header].instructions();
  if (instructions.size() != 3 || instructions[0].opcode() != "phi" ||
      instructions[1].opcode() != "icmp.slt" ||
      instructions[1].operands().size() != 2 ||
      instructions[1].operands()[0] != instructions[0].result() ||
      instructions[2].opcode() != "br" ||
      instructions[2].operands().size() != 3) {
    if (stats)
      std::cerr << "  header shape mismatch "
                << function.blocks()[header].name()
                << " size=" << instructions.size() << '\n';
    return std::nullopt;
  }
  const auto &phi = instructions[0];
  std::size_t latch = function.blocks().size();
  for (const auto predecessor : preds[header]) {
    const auto &candidate = function.blocks()[predecessor].instructions();
    if (candidate.size() >= 2 &&
        isAddOne(candidate[candidate.size() - 2], phi.result()) &&
        candidate.back().opcode() == "br" &&
        candidate.back().operands() ==
            std::vector<std::string>{function.blocks()[header].name()}) {
      if (latch != function.blocks().size())
        return std::nullopt;
      latch = predecessor;
    }
  }
  if (latch == function.blocks().size()) {
    if (stats) {
      std::cerr << "  no latch for " << function.blocks()[header].name()
                << " preds=";
      for (const auto predecessor : preds[header])
        std::cerr << function.blocks()[predecessor].name() << ',';
      std::cerr << '\n';
    }
    return std::nullopt;
  }
  return LoopShape{header, latch, phi.result(), instructions[1].operands()[1]};
}

std::optional<std::vector<std::string>> gepPath(const std::string &address,
                                                const Defs &defs) {
  std::vector<std::string> reversed;
  std::string current = address;
  for (;;) {
    const auto found = defs.find(current);
    if (found == defs.end() || !isGep(*found->second.instruction) ||
        found->second.instruction->operands().size() != 2)
      return std::nullopt;
    const auto &operands = found->second.instruction->operands();
    reversed.push_back(operands[1]);
    current = operands[0];
    if (!current.empty() && current.front() == '@')
      break;
  }
  std::reverse(reversed.begin(), reversed.end());
  return reversed;
}

std::optional<std::string> gepRoot(const std::string &address,
                                   const Defs &defs) {
  std::string current = address;
  for (;;) {
    const auto found = defs.find(current);
    if (found == defs.end() || !isGep(*found->second.instruction) ||
        found->second.instruction->operands().size() != 2)
      return std::nullopt;
    current = found->second.instruction->operands()[0];
    if (!current.empty() && current.front() == '@')
      return current;
  }
}

bool isPlusOne(const std::string &value, const std::string &base,
               const Defs &defs) {
  const auto found = defs.find(value);
  if (found == defs.end())
    return false;
  const auto &instruction = *found->second.instruction;
  return isAddOne(instruction, base);
}

bool replaceUses(Function &function, const std::string &from,
                 const std::string &to) {
  bool changed = false;
  for (auto &block : function.blocks()) {
    for (auto &instruction : block.instructions()) {
      auto operands = instruction.operands();
      bool localChanged = false;
      for (auto &operand : operands) {
        if (operand == from) {
          operand = to;
          changed = true;
          localChanged = true;
        }
      }
      if (localChanged)
        instruction = Instruction{instruction.opcode(), std::move(operands),
                                  instruction.result()};
    }
  }
  return changed;
}

std::optional<Candidate> findCandidate(const Module &module,
                                       const Function &function) {
  const bool stats =
      std::getenv("COMPILER_UNIFORM_INITIALIZED_STENCIL_STATS") != nullptr;
  const auto preds = predecessors(function);
  const auto defs = definitions(function);
  std::unordered_set<std::string> threeDArrays;
  for (const auto &global : module.globals()) {
    if (!global.isConst() && isRankThreeIntArray(global.type()))
      threeDArrays.insert("@" + global.name());
  }
  for (std::size_t innerBody = 0; innerBody < function.blocks().size();
       ++innerBody) {
    const auto &body = function.blocks()[innerBody].instructions();
    bool hasSeedStore = false;
    std::string array;
    std::string seed;
    for (const auto &instruction : body) {
      if (instruction.opcode() != "store" || instruction.operands().size() != 3)
        continue;
      long long value = 0;
      if (!detail::parseIntegerLiteral(instruction.operands()[1], value))
        continue;
      const auto path = gepPath(instruction.operands()[2], defs);
      const auto root = gepRoot(instruction.operands()[2], defs);
      if (!path.has_value() || path->size() != 3 || !root.has_value() ||
          threeDArrays.find(*root) == threeDArrays.end())
        continue;
      if (!array.empty() && array != *root)
        continue;
      hasSeedStore = true;
      if (array.empty())
        array = *root;
      if (seed.empty())
        seed = instruction.operands()[1];
      if (seed != instruction.operands()[1]) {
        hasSeedStore = false;
        break;
      }
    }
    if (!hasSeedStore)
      continue;
    if (stats)
      std::cerr << "uniform-initialized-stencil-elision: seed block "
                << function.blocks()[innerBody].name() << " array=" << array
                << '\n';
    if (function.blocks()[innerBody].terminator() == nullptr)
      continue;
    const auto branch = function.blocks()[innerBody].terminator();
    if (branch->operands().size() != 1)
      continue;
    std::size_t innerHeader = function.blocks().size();
    for (std::size_t index = 0; index < function.blocks().size(); ++index)
      if (function.blocks()[index].name() == branch->operands()[0])
        innerHeader = index;
    if (innerHeader == function.blocks().size()) {
      if (stats)
        std::cerr << "  no inner header\n";
      continue;
    }
    const auto inner = findLoop(function, innerHeader, preds);
    if (!inner.has_value()) {
      if (stats)
        std::cerr << "  inner loop mismatch\n";
      continue;
    }
    std::size_t innerPreheader = function.blocks().size();
    for (auto predecessor : preds[innerHeader])
      if (predecessor != inner->latch)
        innerPreheader = predecessor;
    std::size_t middleHeader = function.blocks().size();
    if (innerPreheader != function.blocks().size()) {
      for (auto predecessor : preds[innerPreheader]) {
        if (findLoop(function, predecessor, preds).has_value()) {
          middleHeader = predecessor;
          break;
        }
      }
    }
    if (middleHeader == function.blocks().size()) {
      if (stats)
        std::cerr << "  no middle header\n";
      continue;
    }
    const auto middle = findLoop(function, middleHeader, preds);
    if (!middle.has_value()) {
      if (stats)
        std::cerr << "  middle loop mismatch\n";
      continue;
    }
    std::size_t middlePreheader = function.blocks().size();
    for (auto predecessor : preds[middleHeader])
      if (predecessor != middle->latch)
        middlePreheader = predecessor;
    std::size_t outerHeader = function.blocks().size();
    if (middlePreheader != function.blocks().size()) {
      for (auto predecessor : preds[middlePreheader]) {
        if (findLoop(function, predecessor, preds).has_value()) {
          outerHeader = predecessor;
          break;
        }
      }
    }
    if (outerHeader == function.blocks().size()) {
      if (stats)
        std::cerr << "  no outer header\n";
      continue;
    }
    const auto outer = findLoop(function, outerHeader, preds);
    if (!outer.has_value()) {
      if (stats)
        std::cerr << "  outer loop mismatch\n";
      continue;
    }
    bool fullSeedCoverage = false;
    for (const auto &instruction : body) {
      if (instruction.opcode() != "store" ||
          instruction.operands().size() != 3 ||
          instruction.operands()[1] != seed)
        continue;
      const auto path = gepPath(instruction.operands()[2], defs);
      const auto root = gepRoot(instruction.operands()[2], defs);
      if (path.has_value() && path->size() == 3 && root.has_value() &&
          *root == array && (*path)[0] == outer->induction &&
          (*path)[1] == middle->induction && (*path)[2] == inner->induction) {
        fullSeedCoverage = true;
        break;
      }
    }
    if (!fullSeedCoverage) {
      if (stats)
        std::cerr << "  seed coverage mismatch\n";
      continue;
    }
    std::unordered_set<std::string> phiValues;
    for (const auto &block : function.blocks())
      for (const auto &instruction : block.instructions())
        if (instruction.opcode() == "phi" && instruction.hasResult())
          phiValues.insert(instruction.result());
    std::string stencilOuter;
    std::string stencilMiddle;
    std::string stencilInner;
    for (std::size_t block = middle->latch + 1;
         block < function.blocks().size() && stencilOuter.empty(); ++block) {
      for (const auto &instruction : function.blocks()[block].instructions()) {
        if (instruction.opcode() != "store" ||
            instruction.operands().size() != 3)
          continue;
        const auto path = gepPath(instruction.operands()[2], defs);
        const auto root = gepRoot(instruction.operands()[2], defs);
        if (!path.has_value() || path->size() != 3 || !root.has_value() ||
            *root != array || !phiValues.count((*path)[0]) ||
            !phiValues.count((*path)[1]) || !phiValues.count((*path)[2]))
          continue;
        stencilOuter = (*path)[0];
        stencilMiddle = (*path)[1];
        stencilInner = (*path)[2];
        break;
      }
    }
    if (stencilOuter.empty()) {
      if (stats)
        std::cerr << "  no stencil store\n";
      continue;
    }
    // The initialization block itself must be effect-free apart from uniform
    // seed stores.  Other arrays may be initialized alongside the candidate.
    bool effectsClosed = true;
    for (const auto &instruction : body) {
      if (instruction.opcode() == "call" || instruction.opcode() == "load") {
        effectsClosed = false;
        break;
      }
      if (instruction.opcode() == "store" &&
          instruction.operands().size() == 3) {
        const auto root = gepRoot(instruction.operands()[2], defs);
        if (root.has_value() && *root == array &&
            instruction.operands()[1] != seed) {
          effectsClosed = false;
          break;
        }
      }
    }
    if (!effectsClosed)
      continue;
    return Candidate{outer->header,
                     middle->header,
                     inner->header,
                     middle->latch,
                     array,
                     seed,
                     outer->induction,
                     middle->induction,
                     inner->induction,
                     outer->bound,
                     stencilOuter,
                     stencilMiddle,
                     stencilInner};
  }
  return std::nullopt;
}

void applyCandidate(Function &function, const Candidate &candidate) {
  auto defs = definitions(function);
  auto &innerHeader = function.blocks()[candidate.innerHeader].instructions();
  const auto fresh = [&](const std::string &stem) {
    std::unordered_set<std::string> names;
    for (const auto &block : function.blocks())
      for (const auto &instruction : block.instructions())
        if (instruction.hasResult())
          names.insert(instruction.result());
    for (std::size_t suffix = 0;; ++suffix) {
      const auto name =
          "%uniform.stencil." + stem + "." + std::to_string(suffix);
      if (names.find(name) == names.end())
        return name;
    }
  };
  const auto outerZeroInner = fresh("inner.outer.zero");
  const auto middleZeroInner = fresh("inner.middle.zero");
  const auto fullInner = fresh("inner.full");
  const auto innerBound = fresh("inner.bound");
  innerHeader.insert(
      innerHeader.end() - 2,
      Instruction{"icmp.eq", {candidate.outer, "0"}, outerZeroInner});
  innerHeader.insert(
      innerHeader.end() - 2,
      Instruction{"icmp.eq", {candidate.middle, "0"}, middleZeroInner});
  innerHeader.insert(
      innerHeader.end() - 2,
      Instruction{"or", {outerZeroInner, middleZeroInner}, fullInner});
  innerHeader.insert(
      innerHeader.end() - 2,
      Instruction{"select", {fullInner, candidate.bound, "1"}, innerBound});
  for (auto &instruction : innerHeader)
    if (instruction.opcode() == "icmp.slt" &&
        instruction.operands().size() == 2 &&
        instruction.operands()[0] == candidate.inner)
      instruction = Instruction{
          "icmp.slt", {candidate.inner, innerBound}, instruction.result()};

  // Materialize the high boundary once per row.  This is needed by putarray
  // after the stencil; all other future cells are synthesized as the seed.
  auto &middleLatch = function.blocks()[candidate.middleLatch].instructions();
  const auto rowBase = fresh("row.base");
  const auto rowMiddle = fresh("row.middle");
  const auto lastIndex = fresh("last.index");
  const auto lastAddress = fresh("last.address");
  middleLatch.insert(middleLatch.end() - 2,
                     Instruction{"getelementptr",
                                 {candidate.array, candidate.outer},
                                 rowBase});
  middleLatch.insert(
      middleLatch.end() - 2,
      Instruction{"getelementptr", {rowBase, candidate.middle}, rowMiddle});
  middleLatch.insert(middleLatch.end() - 2,
                     Instruction{"sub", {candidate.bound, "1"}, lastIndex});
  middleLatch.insert(
      middleLatch.end() - 2,
      Instruction{"getelementptr", {rowMiddle, lastIndex}, lastAddress});
  middleLatch.insert(
      middleLatch.end() - 2,
      Instruction{"store", {"int", candidate.seed, lastAddress}});

  defs = definitions(function);
  for (auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "load" ||
          instruction.operands().size() != 2 || !instruction.hasResult())
        continue;
      const auto path = gepPath(instruction.operands()[1], defs);
      const auto root = gepRoot(instruction.operands()[1], defs);
      if (!path.has_value() || path->size() != 3 || !root.has_value() ||
          *root != candidate.array)
        continue;
      const auto &indices = *path;
      const bool futureOuter =
          isPlusOne(indices[0], candidate.stencilOuter, defs) &&
          indices[1] == candidate.stencilMiddle &&
          indices[2] == candidate.stencilInner;
      const bool futureMiddle =
          indices[0] == candidate.stencilOuter &&
          isPlusOne(indices[1], candidate.stencilMiddle, defs) &&
          indices[2] == candidate.stencilInner;
      const bool futureInner =
          indices[0] == candidate.stencilOuter &&
          indices[1] == candidate.stencilMiddle &&
          isPlusOne(indices[2], candidate.stencilInner, defs);
      if (futureOuter || futureMiddle || futureInner)
        replaceUses(function, instruction.result(), candidate.seed);
    }
  }
}

} // namespace

void UniformInitializedStencilElisionPass::run(Module &module) {
  if (std::getenv("COMPILER_DISABLE_UNIFORM_INITIALIZED_STENCIL_ELISION") !=
      nullptr)
    return;
  for (auto &function : module.functions()) {
    const auto candidate = findCandidate(module, function);
    if (candidate.has_value()) {
      if (std::getenv("COMPILER_UNIFORM_INITIALIZED_STENCIL_STATS") !=
          nullptr) {
        std::cerr << "uniform-initialized-stencil-elision: outer="
                  << function.blocks()[candidate->outerHeader].name()
                  << " middle="
                  << function.blocks()[candidate->middleHeader].name()
                  << " inner="
                  << function.blocks()[candidate->innerHeader].name()
                  << " array=" << candidate->array
                  << " stencil=" << candidate->stencilOuter << ","
                  << candidate->stencilMiddle << "," << candidate->stencilInner
                  << '\n';
      }
      applyCandidate(function, *candidate);
      module.addPassNote("uniform-initialized-stencil-elision");
      break;
    }
  }
}

} // namespace compiler::midend::pass
