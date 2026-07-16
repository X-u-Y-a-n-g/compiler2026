#include "../../../include/compiler/backend/opt/MachineTraceBlockPlacement.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::opt {

namespace {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::target::TargetInstrAdapter;

constexpr std::size_t kMaxAnalyzedInstructions = 4096;

bool allBlocksHaveTerminators(const MachineFunction& function,
                              const TargetInstrAdapter& adapter) {
  return std::all_of(function.blocks.begin(), function.blocks.end(),
                     [&](const MachineBlock& block) {
                       if (block.instructions.empty()) return false;
                       const auto& terminator = block.instructions.back();
                       return adapter.isConditionalBranch(terminator) ||
                              adapter.isUnconditionalBranch(terminator) ||
                              adapter.isReturn(terminator);
                     });
}

std::size_t controlTransfers(const MachineFunction& function,
                             const TargetInstrAdapter& adapter) {
  std::size_t result = 0;
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    const auto& block = function.blocks[index];
    if (block.instructions.empty()) continue;
    const auto& terminator = block.instructions.back();
    const auto targets = adapter.branchTargets(terminator);
    const std::optional<std::string> fallthrough =
        index + 1 < function.blocks.size()
            ? std::optional<std::string>{function.blocks[index + 1].name}
            : std::nullopt;
    if (adapter.isUnconditionalBranch(terminator)) {
      if (targets.size() == 1 &&
          (!fallthrough.has_value() || targets.front() != *fallthrough)) {
        ++result;
      }
      continue;
    }
    if (adapter.isConditionalBranch(terminator) && targets.size() == 2) {
      ++result;
      if (!fallthrough.has_value() ||
          (targets[0] != *fallthrough && targets[1] != *fallthrough)) {
        ++result;
      }
    }
  }
  return result;
}

bool moveAfter(MachineFunction& function, const std::string& moving,
               const std::string& anchor) {
  if (moving == anchor || function.blocks.empty() ||
      function.blocks.front().name == moving) {
    return false;
  }
  std::optional<std::size_t> movingIndex;
  std::optional<std::size_t> anchorIndex;
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    if (function.blocks[index].name == moving) movingIndex = index;
    if (function.blocks[index].name == anchor) anchorIndex = index;
  }
  if (!movingIndex.has_value() || !anchorIndex.has_value() ||
      *movingIndex == *anchorIndex + 1) {
    return false;
  }
  auto block = std::move(function.blocks[*movingIndex]);
  function.blocks.erase(function.blocks.begin() +
                        static_cast<std::ptrdiff_t>(*movingIndex));
  if (*movingIndex < *anchorIndex) --*anchorIndex;
  function.blocks.insert(
      function.blocks.begin() + static_cast<std::ptrdiff_t>(*anchorIndex + 1),
      std::move(block));
  return true;
}

bool improveOnce(MachineFunction& function,
                 const TargetInstrAdapter& adapter) {
  const auto baseline = controlTransfers(function, adapter);
  std::size_t best = baseline;
  std::vector<MachineBlock> bestBlocks;

  // Use stable names because trial moves invalidate block indices.
  std::vector<std::string> anchors;
  anchors.reserve(function.blocks.size());
  for (const auto& block : function.blocks) anchors.push_back(block.name);

  for (const auto& anchor : anchors) {
    const auto found = std::find_if(
        function.blocks.begin(), function.blocks.end(),
        [&](const MachineBlock& block) { return block.name == anchor; });
    if (found == function.blocks.end() || found->instructions.empty()) continue;
    const auto targets = adapter.branchTargets(found->instructions.back());
    for (const auto& target : targets) {
      auto trial = function;
      if (!moveAfter(trial, target, anchor)) continue;
      const auto trialTransfers = controlTransfers(trial, adapter);
      if (trialTransfers < best) {
        best = trialTransfers;
        bestBlocks = std::move(trial.blocks);
      }
    }
  }

  if (best >= baseline) return false;
  function.blocks = std::move(bestBlocks);
  return true;
}

}  // namespace

void MachineTraceBlockPlacement::run(
    compiler::backend::mir::MachineModule& module) const {
  const auto& adapter =
      compiler::backend::target::instrAdapterFor(module.target);
  for (auto& function : module.functions) {
    std::size_t instructionCount = 0;
    for (const auto& block : function.blocks) {
      instructionCount += block.instructions.size();
    }
    if (instructionCount > kMaxAnalyzedInstructions ||
        !allBlocksHaveTerminators(function, adapter)) {
      continue;
    }
    while (improveOnce(function, adapter)) {
    }
  }
}

}  // namespace compiler::backend::opt
