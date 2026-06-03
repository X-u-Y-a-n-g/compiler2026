#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../include/compiler/backend/mir/MachineModule.h"
#include "../../../include/compiler/backend/target/TargetInstrAdapter.h"

namespace compiler::backend::opt::detail {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineInstructionKind;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;

inline bool isRegisterLike(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister ||
         operand.kind == MachineOperandKind::PhysicalRegister;
}

inline bool isVirtualRegister(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::VirtualRegister;
}

inline bool isImmediateInt(const MachineOperand& operand) {
  return operand.kind == MachineOperandKind::ImmediateInt;
}

inline bool sameOperand(const MachineOperand& lhs, const MachineOperand& rhs) {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  switch (lhs.kind) {
    case MachineOperandKind::None:
      return true;
    case MachineOperandKind::ImmediateInt:
      return lhs.intValue == rhs.intValue;
    case MachineOperandKind::ImmediateFloat:
      return lhs.floatValue == rhs.floatValue;
    case MachineOperandKind::VirtualRegister:
    case MachineOperandKind::PhysicalRegister:
    case MachineOperandKind::GlobalAddress:
    case MachineOperandKind::FunctionSymbol:
    case MachineOperandKind::Label:
      return lhs.text == rhs.text;
    case MachineOperandKind::FrameIndex:
    case MachineOperandKind::StackSlot:
    case MachineOperandKind::StackObject:
      return lhs.index == rhs.index;
  }
  return false;
}

inline std::string regKey(const MachineOperand& operand) {
  if (!isRegisterLike(operand)) {
    return {};
  }
  return (operand.kind == MachineOperandKind::VirtualRegister ? "v:" : "p:") + operand.text;
}

inline std::string operandKey(const MachineOperand& operand) {
  switch (operand.kind) {
    case MachineOperandKind::None:
      return "none";
    case MachineOperandKind::ImmediateInt:
      return "ii:" + std::to_string(operand.intValue);
    case MachineOperandKind::ImmediateFloat:
      return "if:" + std::to_string(operand.floatValue);
    case MachineOperandKind::VirtualRegister:
      return "vr:" + operand.text;
    case MachineOperandKind::PhysicalRegister:
      return "pr:" + operand.text;
    case MachineOperandKind::FrameIndex:
      return "fi:" + std::to_string(operand.index);
    case MachineOperandKind::StackSlot:
      return "ss:" + std::to_string(operand.index);
    case MachineOperandKind::StackObject:
      return "so:" + std::to_string(operand.index);
    case MachineOperandKind::GlobalAddress:
      return "ga:" + operand.text;
    case MachineOperandKind::FunctionSymbol:
      return "fn:" + operand.text;
    case MachineOperandKind::Label:
      return "lb:" + operand.text;
  }
  return {};
}

inline bool opcodeIsMemoryAccess(const std::string& opcode) {
  return opcode == "load" || opcode == "store" || opcode == "frame.load" ||
         opcode == "frame.store" || opcode == "spill.reload" || opcode == "spill.store" ||
         opcode == "stack.param.load" || opcode == "stack.save" ||
         opcode == "stack.restore";
}

inline bool opcodeIsPureTarget(const std::string& opcode) {
  return opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
         opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
         opcode == "or" || opcode == "neg" || opcode == "fneg" || opcode == "fadd" ||
         opcode == "fsub" || opcode == "fmul" || opcode == "fdiv" || opcode == "cast" ||
         opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
         opcode == "sitofp" || opcode == "uitofp" || opcode == "fptosi" ||
         opcode == "gep" || opcode == "gep.linear" || opcode == "getelementptr" ||
         opcode == "getelementptr.linear" || opcode.rfind("icmp.", 0) == 0 ||
         opcode.rfind("fcmp.", 0) == 0;
}

inline bool isSideEffecting(const MachineInstruction& instruction,
                            const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (adapter.isCall(instruction) || adapter.isReturn(instruction) ||
      adapter.isConditionalBranch(instruction) || adapter.isUnconditionalBranch(instruction)) {
    return true;
  }
  if (instruction.kind == MachineInstructionKind::SpillStore ||
      instruction.kind == MachineInstructionKind::FrameStore ||
      instruction.kind == MachineInstructionKind::StackAdjust ||
      instruction.kind == MachineInstructionKind::StackSetFramePointer ||
      instruction.kind == MachineInstructionKind::StackSave ||
      instruction.kind == MachineInstructionKind::StackRestore ||
      instruction.kind == MachineInstructionKind::StackParameterLoad) {
    return true;
  }
  return instruction.opcode == "store" || instruction.opcode == "stack.adjust" ||
         instruction.opcode == "stack.set_fp" || instruction.opcode == "stack.save" ||
         instruction.opcode == "stack.restore" || instruction.opcode == "stack.param.load";
}

inline bool isPureInstruction(const MachineInstruction& instruction,
                              const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (!instruction.hasResult || !instruction.result.isValid() || isSideEffecting(instruction, adapter)) {
    return false;
  }
  if (instruction.kind == MachineInstructionKind::Copy) {
    return true;
  }
  return instruction.kind == MachineInstructionKind::Target && opcodeIsPureTarget(instruction.opcode);
}

inline std::vector<MachineOperand> usesOf(
    const MachineInstruction& instruction,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::vector<MachineOperand> uses;
  adapter.enumerateUses(instruction, uses);
  return uses;
}

inline std::vector<MachineOperand> defsOf(
    const MachineInstruction& instruction,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::vector<MachineOperand> defs;
  adapter.enumerateDefs(instruction, defs);
  return defs;
}

inline void replaceUseByKey(MachineInstruction& instruction, const std::string& fromKey,
                            MachineOperand to,
                            const compiler::backend::target::TargetInstrAdapter& adapter) {
  if (fromKey.size() < 3) {
    return;
  }
  adapter.replaceUse(instruction, fromKey.substr(2), std::move(to));
}

inline void removeDeadInstructionsByMask(MachineBlock& block,
                                         const std::vector<bool>& removeInstruction) {
  std::vector<MachineInstruction> kept;
  kept.reserve(block.instructions.size());
  for (std::size_t i = 0; i < block.instructions.size(); ++i) {
    if (!removeInstruction[i]) {
      kept.push_back(std::move(block.instructions[i]));
    }
  }
  block.instructions = std::move(kept);
}

inline bool checkedBinary(const std::string& opcode, std::int64_t lhs, std::int64_t rhs,
                          std::int64_t& out) {
  if (opcode == "add") {
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
      return false;
    }
    out = lhs + rhs;
    return true;
  }
  if (opcode == "sub") {
    if ((rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs) ||
        (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs)) {
      return false;
    }
    out = lhs - rhs;
    return true;
  }
  if (opcode == "mul") {
    const auto product = static_cast<long double>(lhs) * static_cast<long double>(rhs);
    if (product > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        product < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
      return false;
    }
    out = lhs * rhs;
    return true;
  }
  if (opcode == "div" || opcode == "sdiv") {
    if (rhs == 0 || (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1)) {
      return false;
    }
    out = lhs / rhs;
    return true;
  }
  if (opcode == "mod" || opcode == "srem") {
    if (rhs == 0 || (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1)) {
      return false;
    }
    out = lhs % rhs;
    return true;
  }
  if (opcode == "and") {
    out = lhs & rhs;
    return true;
  }
  if (opcode == "or") {
    out = lhs | rhs;
    return true;
  }
  return false;
}

inline bool checkedCompare(const std::string& opcode, std::int64_t lhs, std::int64_t rhs,
                           std::int64_t& out) {
  if (opcode == "icmp.eq") {
    out = lhs == rhs;
  } else if (opcode == "icmp.ne") {
    out = lhs != rhs;
  } else if (opcode == "icmp.slt") {
    out = lhs < rhs;
  } else if (opcode == "icmp.sle") {
    out = lhs <= rhs;
  } else if (opcode == "icmp.sgt") {
    out = lhs > rhs;
  } else if (opcode == "icmp.sge") {
    out = lhs >= rhs;
  } else {
    return false;
  }
  return true;
}

inline std::vector<std::size_t> reachableBlocks(
    const MachineFunction& function,
    const compiler::backend::target::TargetInstrAdapter& adapter) {
  std::unordered_map<std::string, std::size_t> indexByName;
  for (std::size_t i = 0; i < function.blocks.size(); ++i) {
    indexByName.emplace(function.blocks[i].name, i);
  }

  std::vector<std::size_t> order;
  std::vector<std::size_t> stack;
  std::vector<bool> seen(function.blocks.size(), false);
  if (!function.blocks.empty()) {
    stack.push_back(0);
  }

  while (!stack.empty()) {
    const auto index = stack.back();
    stack.pop_back();
    if (index >= function.blocks.size() || seen[index]) {
      continue;
    }
    seen[index] = true;
    order.push_back(index);

    const auto& block = function.blocks[index];
    std::vector<std::size_t> successors;
    if (!block.instructions.empty()) {
      const auto& terminator = block.instructions.back();
      if (adapter.isConditionalBranch(terminator) || adapter.isUnconditionalBranch(terminator)) {
        for (const auto& target : adapter.branchTargets(terminator)) {
          const auto found = indexByName.find(target);
          if (found != indexByName.end()) {
            successors.push_back(found->second);
          }
        }
      } else if (!adapter.isReturn(terminator) && index + 1 < function.blocks.size()) {
        successors.push_back(index + 1);
      }
    } else if (index + 1 < function.blocks.size()) {
      successors.push_back(index + 1);
    }

    for (auto it = successors.rbegin(); it != successors.rend(); ++it) {
      if (!seen[*it]) {
        stack.push_back(*it);
      }
    }
  }
  return order;
}

}  // namespace compiler::backend::opt::detail
