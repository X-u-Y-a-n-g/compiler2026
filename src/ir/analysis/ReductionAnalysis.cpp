#include "../../../include/compiler/ir/analysis/ReductionAnalysis.h"

#include <optional>
#include <unordered_map>

#include "../../../include/compiler/ir/analysis/UseDefAnalysis.h"
#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Module.h"

namespace compiler::ir::analysis {
namespace {

using compiler::ir::core::Instruction;

std::optional<std::string> phiIncoming(const Instruction& phi,
                                       const std::string& predecessor) {
  if (phi.opcode() != "phi") return std::nullopt;
  for (std::size_t index = 1; index + 1 < phi.operands().size(); index += 2) {
    if (phi.operands()[index + 1] == predecessor) return phi.operands()[index];
  }
  return std::nullopt;
}

std::optional<ReductionKind> reductionKind(const std::string& opcode) {
  if (opcode == "add" || opcode == "fadd") return ReductionKind::Add;
  if (opcode == "mul" || opcode == "fmul") return ReductionKind::Multiply;
  if (opcode == "xor") return ReductionKind::Xor;
  if (opcode == "and") return ReductionKind::And;
  if (opcode == "or") return ReductionKind::Or;
  if (opcode == "min" || opcode == "smin" || opcode == "umin" ||
      opcode == "fmin") {
    return ReductionKind::Minimum;
  }
  if (opcode == "max" || opcode == "smax" || opcode == "umax" ||
      opcode == "fmax") {
    return ReductionKind::Maximum;
  }
  return std::nullopt;
}

}  // namespace

ReductionAnalysis::ReductionAnalysis(const compiler::ir::core::Module& module,
                                     const compiler::ir::core::Function& function,
                                     const LoopAnalysis& loops) {
  const UseDefAnalysis useDef(module, function);
  std::unordered_map<std::string, const Instruction*> definitions;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) definitions.emplace(instruction.result(), &instruction);
    }
  }

  for (const auto& loop : loops.loops()) {
    if (!loop.preheader.has_value() || !loop.hasUniqueLatch) continue;
    const auto& preheader = function.blocks()[*loop.preheader].name();
    const auto& latch = function.blocks()[loop.latches.front()].name();
    for (const auto& phi : function.blocks()[loop.header].instructions()) {
      if (phi.opcode() != "phi" || !phi.hasResult() || phi.operands().empty()) continue;
      if (loop.induction.has_value() && loop.induction->phi == phi.result()) continue;
      const auto initial = phiIncoming(phi, preheader);
      const auto update = phiIncoming(phi, latch);
      if (!initial.has_value() || !update.has_value()) continue;
      const auto updateFound = definitions.find(*update);
      if (updateFound == definitions.end()) continue;
      const auto kind = reductionKind(updateFound->second->opcode());
      if (!kind.has_value() || updateFound->second->operands().size() != 2) continue;
      const auto& operands = updateFound->second->operands();
      std::string input;
      if (operands[0] == phi.result()) {
        input = operands[1];
      } else if (operands[1] == phi.result()) {
        input = operands[0];
      } else {
        continue;
      }
      if (useDef.useCount(phi.result()) < 2) continue;
      const bool floating = updateFound->second->opcode().front() == 'f';
      reductions_.push_back(LoopReduction{
          *kind, loop.header, phi.operands()[0], phi.result(), *initial,
          *update, input, floating, !floating});
    }
  }
}

std::vector<LoopReduction> ReductionAnalysis::reductionsForLoop(
    std::size_t loopHeader) const {
  std::vector<LoopReduction> result;
  for (const auto& reduction : reductions_) {
    if (reduction.loopHeader == loopHeader) result.push_back(reduction);
  }
  return result;
}

}  // namespace compiler::ir::analysis
