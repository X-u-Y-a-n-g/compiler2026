#include "compiler/backend/stack/StackLowering.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "compiler/backend/target/TargetBackend.h"

namespace compiler::backend::stack {

namespace {

using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::ValueType;

std::size_t alignTo(std::size_t value, std::size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : (value + alignment - remainder);
}

MachineInstruction makeStackAdjust(long long delta) {
  MachineInstruction instruction;
  instruction.kind = compiler::backend::mir::MachineInstructionKind::StackAdjust;
  instruction.opcode = "stack.adjust";
  instruction.operands.push_back(MachineOperand::immediateInt(delta, ValueType::int64Type()));
  return instruction;
}

MachineInstruction makeStackSave(const std::string& reg, const ValueType& type, long long offset) {
  MachineInstruction instruction;
  instruction.kind = compiler::backend::mir::MachineInstructionKind::StackSave;
  instruction.opcode = "stack.save";
  instruction.operands.push_back(MachineOperand::physicalRegister(reg, type));
  instruction.operands.push_back(MachineOperand::immediateInt(offset, ValueType::int64Type()));
  return instruction;
}

MachineInstruction makeStackRestore(const std::string& reg, const ValueType& type, long long offset) {
  MachineInstruction instruction;
  instruction.kind = compiler::backend::mir::MachineInstructionKind::StackRestore;
  instruction.opcode = "stack.restore";
  instruction.hasResult = true;
  instruction.result = MachineOperand::physicalRegister(reg, type);
  instruction.operands.push_back(MachineOperand::immediateInt(offset, ValueType::int64Type()));
  return instruction;
}

MachineInstruction makeParamCopy(int homeSlot, MachineOperand source, const ValueType& type) {
  return MachineInstruction::frameStore(std::move(source), MachineOperand::stackSlot(homeSlot, type));
}

MachineInstruction makeIncomingStackParamLoad(int homeSlot, const ValueType& type, long long offset) {
  MachineInstruction instruction;
  instruction.kind = compiler::backend::mir::MachineInstructionKind::StackParameterLoad;
  instruction.opcode = "stack.param.load";
  instruction.hasResult = true;
  instruction.result = MachineOperand::stackSlot(homeSlot, type);
  instruction.operands.push_back(MachineOperand::immediateInt(offset, ValueType::int64Type()));
  return instruction;
}

void collectUsedFloatCalleeSaved(
    MachineFunction& function, const compiler::backend::target::TargetRegInfo& regInfo) {
  std::vector<std::string> used;
  std::vector<MachineOperand> operands;
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instruction.hasResult) {
        operands.push_back(instruction.result);
      }
      operands.insert(operands.end(), instruction.operands.begin(), instruction.operands.end());
    }
  }

  for (const auto& reg : regInfo.calleeSavedFloatRegisters()) {
    const auto usedInFunction = std::any_of(operands.begin(), operands.end(), [&](const auto& operand) {
      return operand.kind == compiler::backend::mir::MachineOperandKind::PhysicalRegister &&
             operand.text == reg;
    });
    if (usedInFunction) {
      used.push_back(reg);
    }
  }
  function.usedFloatCalleeSavedRegisters = std::move(used);
}

}  // namespace

void StackLowering::lower(compiler::backend::mir::MachineModule& module) const {
  const auto& regInfo = compiler::backend::target::regInfoFor(module.target);

  for (auto& function : module.functions) {
    collectUsedFloatCalleeSaved(function, regInfo);

    function.savedRaOffset = static_cast<int>(module.target.pointerSize());
    function.savedFpOffset = static_cast<int>(module.target.pointerSize() * 2);

    function.savedIntegerRegisterOffsets.clear();
    function.savedIntegerRegisterOffsets.reserve(function.usedIntegerCalleeSavedRegisters.size());
    for (std::size_t i = 0; i < function.usedIntegerCalleeSavedRegisters.size(); ++i) {
      function.savedIntegerRegisterOffsets.push_back(
          static_cast<int>(module.target.pointerSize() * (3 + i)));
    }

    function.savedFloatRegisterOffsets.clear();
    const std::size_t floatSaveBase =
        module.target.pointerSize() * (3 + function.usedIntegerCalleeSavedRegisters.size());
    for (std::size_t i = 0; i < function.usedFloatCalleeSavedRegisters.size(); ++i) {
      function.savedFloatRegisterOffsets.push_back(static_cast<int>(floatSaveBase + 8 * i));
    }

    const std::size_t saveArea = module.target.pointerSize() * 2 +
                                 module.target.pointerSize() *
                                     function.usedIntegerCalleeSavedRegisters.size() +
                                 8 * function.usedFloatCalleeSavedRegisters.size();

    std::size_t cursor = saveArea;
    for (auto& object : function.stackObjects) {
      cursor = alignTo(cursor, object.alignment);
      cursor += object.size;
      object.offset = static_cast<int>(cursor);
    }
    cursor += function.outgoingArgSize;
    function.frameSize = alignTo(cursor, module.target.stackAlignment());
    if (function.frameSize == 0) {
      function.frameSize = module.target.stackAlignment();
    }

    function.prologueInstructions.clear();
    function.epilogueInstructions.clear();

    function.prologueInstructions.push_back(makeStackAdjust(-static_cast<long long>(function.frameSize)));
    function.prologueInstructions.push_back(
        makeStackSave(regInfo.returnAddressRegister(), ValueType::int64Type(),
                      static_cast<long long>(function.frameSize - module.target.pointerSize())));
    function.prologueInstructions.push_back(
        makeStackSave(regInfo.framePointerRegister(), ValueType::int64Type(),
                      static_cast<long long>(function.frameSize - module.target.pointerSize() * 2)));
    for (std::size_t i = 0; i < function.usedIntegerCalleeSavedRegisters.size(); ++i) {
      function.prologueInstructions.push_back(
          makeStackSave(function.usedIntegerCalleeSavedRegisters[i], ValueType::int64Type(),
                        static_cast<long long>(function.frameSize -
                                               function.savedIntegerRegisterOffsets[i])));
    }
    for (std::size_t i = 0; i < function.usedFloatCalleeSavedRegisters.size(); ++i) {
      function.prologueInstructions.push_back(
          makeStackSave(function.usedFloatCalleeSavedRegisters[i], ValueType::float32Type(),
                        static_cast<long long>(function.frameSize -
                                               function.savedFloatRegisterOffsets[i])));
    }

    MachineInstruction setFramePointer;
    setFramePointer.kind = compiler::backend::mir::MachineInstructionKind::StackSetFramePointer;
    setFramePointer.opcode = "stack.set_fp";
    setFramePointer.operands.push_back(
        MachineOperand::immediateInt(static_cast<long long>(function.frameSize), ValueType::int64Type()));
    function.prologueInstructions.push_back(std::move(setFramePointer));

    std::size_t intRegs = 0;
    std::size_t floatRegs = 0;
    std::size_t stackOffset = 0;
    for (const auto& parameter : function.parameters) {
      if (parameter.homeSlot < 0) {
        continue;
      }
      if (parameter.type.isFloat()) {
        if (floatRegs < regInfo.floatArgumentRegisters().size()) {
          function.prologueInstructions.push_back(
              makeParamCopy(parameter.homeSlot,
                            MachineOperand::physicalRegister(regInfo.floatArgumentRegisters()[floatRegs],
                                                            parameter.type),
                            parameter.type));
          ++floatRegs;
        } else {
          function.prologueInstructions.push_back(
              makeIncomingStackParamLoad(parameter.homeSlot, parameter.type,
                                         static_cast<long long>(stackOffset)));
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      } else {
        if (intRegs < regInfo.integerArgumentRegisters().size()) {
          function.prologueInstructions.push_back(
              makeParamCopy(parameter.homeSlot,
                            MachineOperand::physicalRegister(regInfo.integerArgumentRegisters()[intRegs],
                                                            parameter.type),
                            parameter.type));
          ++intRegs;
        } else {
          function.prologueInstructions.push_back(
              makeIncomingStackParamLoad(parameter.homeSlot, parameter.type,
                                         static_cast<long long>(stackOffset)));
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      }
    }

    for (std::size_t i = function.usedFloatCalleeSavedRegisters.size(); i > 0; --i) {
      function.epilogueInstructions.push_back(
          makeStackRestore(function.usedFloatCalleeSavedRegisters[i - 1], ValueType::float32Type(),
                           -function.savedFloatRegisterOffsets[i - 1]));
    }
    for (std::size_t i = function.usedIntegerCalleeSavedRegisters.size(); i > 0; --i) {
      function.epilogueInstructions.push_back(
          makeStackRestore(function.usedIntegerCalleeSavedRegisters[i - 1], ValueType::int64Type(),
                           -function.savedIntegerRegisterOffsets[i - 1]));
    }
    function.epilogueInstructions.push_back(
        makeStackRestore(regInfo.returnAddressRegister(), ValueType::int64Type(),
                         -function.savedRaOffset));
    function.epilogueInstructions.push_back(
        makeStackRestore(regInfo.framePointerRegister(), ValueType::int64Type(),
                         -function.savedFpOffset));
    function.epilogueInstructions.push_back(
        makeStackAdjust(static_cast<long long>(function.frameSize)));
  }
}

}  // namespace compiler::backend::stack
