#include "../../../include/compiler/backend/stack/StackLowering.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "../../../include/compiler/backend/target/TargetBackend.h"

namespace compiler::backend::stack {

namespace {

using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
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

bool instructionUsesPhysicalRegister(const MachineInstruction& instruction,
                                     const std::string& reg) {
  if (instruction.hasResult &&
      instruction.result.kind == MachineOperandKind::PhysicalRegister &&
      instruction.result.text == reg) {
    return true;
  }
  for (const auto& operand : instruction.operands) {
    if (operand.kind == MachineOperandKind::PhysicalRegister && operand.text == reg) {
      return true;
    }
  }
  for (const auto& incoming : instruction.phiInputs) {
    if (incoming.value.kind == MachineOperandKind::PhysicalRegister &&
        incoming.value.text == reg) {
      return true;
    }
  }
  return false;
}

bool hasDynamicStackAdjustment(const MachineFunction& function) {
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instruction.kind ==
              compiler::backend::mir::MachineInstructionKind::StackAdjust ||
          instruction.opcode == "stack.adjust") {
        return true;
      }
    }
  }
  return false;
}

bool bodyUsesFramePointer(const MachineFunction& function,
                          const std::string& framePointer) {
  for (const auto& block : function.blocks) {
    for (const auto& instruction : block.instructions) {
      if (instructionUsesPhysicalRegister(instruction, framePointer)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void StackLowering::lower(compiler::backend::mir::MachineModule& module) const {
  const auto& regInfo = compiler::backend::target::regInfoFor(module.target);

  for (auto& function : module.functions) {
    collectUsedFloatCalleeSaved(function, regInfo);

    const bool hasCall = std::any_of(
        function.blocks.begin(), function.blocks.end(), [](const auto& block) {
          return std::any_of(block.instructions.begin(), block.instructions.end(),
                             [](const auto& instruction) {
                               return instruction.opcode == "call";
                             });
        });
    const bool hasStackObject =
        std::any_of(function.stackObjects.begin(), function.stackObjects.end(),
                    [](const auto& object) { return object.size != 0; });
    const bool hasStackParameter =
        std::any_of(function.parameters.begin(), function.parameters.end(),
                    [](const auto& parameter) {
                      return parameter.homeSlot >= 0;
                    });
    const bool canOmitFrame =
        !hasCall && !hasStackObject && !hasStackParameter &&
        function.outgoingArgSize == 0 &&
        function.usedIntegerCalleeSavedRegisters.empty() &&
        function.usedFloatCalleeSavedRegisters.empty();
    if (canOmitFrame) {
      function.frameSize = 0;
      function.usesFramePointer = false;
      function.savedRaOffset = 0;
      function.savedFpOffset = 0;
      function.savedIntegerRegisterOffsets.clear();
      function.savedFloatRegisterOffsets.clear();
      function.prologueInstructions.clear();
      function.epilogueInstructions.clear();
      continue;
    }

    // With a fixed stack pointer, SP is a stable frame base for every function,
    // not only directly recursive ones.  Keep FP only when the body changes SP
    // dynamically or already contains an explicit reference to the target FP.
    function.usesFramePointer =
        hasDynamicStackAdjustment(function) ||
        bodyUsesFramePointer(function, regInfo.framePointerRegister());

    function.savedRaOffset = static_cast<int>(module.target.pointerSize());
    function.savedFpOffset =
        function.usesFramePointer ? static_cast<int>(module.target.pointerSize() * 2) : 0;

    const std::size_t fixedSaveCount = function.usesFramePointer ? 2 : 1;

    function.savedIntegerRegisterOffsets.clear();
    function.savedIntegerRegisterOffsets.reserve(function.usedIntegerCalleeSavedRegisters.size());
    for (std::size_t i = 0; i < function.usedIntegerCalleeSavedRegisters.size(); ++i) {
      function.savedIntegerRegisterOffsets.push_back(
          static_cast<int>(module.target.pointerSize() * (fixedSaveCount + 1 + i)));
    }

    function.savedFloatRegisterOffsets.clear();
    const std::size_t floatSaveBase =
        module.target.pointerSize() *
        (fixedSaveCount + 1 + function.usedIntegerCalleeSavedRegisters.size());
    for (std::size_t i = 0; i < function.usedFloatCalleeSavedRegisters.size(); ++i) {
      function.savedFloatRegisterOffsets.push_back(static_cast<int>(floatSaveBase + 8 * i));
    }

    const std::size_t saveArea = module.target.pointerSize() * fixedSaveCount +
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
    if (function.usesFramePointer) {
      function.prologueInstructions.push_back(
          makeStackSave(regInfo.framePointerRegister(), ValueType::int64Type(),
                        static_cast<long long>(function.frameSize -
                                               module.target.pointerSize() * 2)));
    }
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

    if (function.usesFramePointer) {
      MachineInstruction setFramePointer;
      setFramePointer.kind = compiler::backend::mir::MachineInstructionKind::StackSetFramePointer;
      setFramePointer.opcode = "stack.set_fp";
      setFramePointer.operands.push_back(MachineOperand::immediateInt(
          static_cast<long long>(function.frameSize), ValueType::int64Type()));
      function.prologueInstructions.push_back(std::move(setFramePointer));
    }

    std::size_t intRegs = 0;
    std::size_t floatRegs = 0;
    std::size_t stackOffset = 0;
    for (const auto& parameter : function.parameters) {
      if (parameter.type.isFloat()) {
        if (floatRegs < regInfo.floatArgumentRegisters().size()) {
          if (parameter.homeSlot >= 0) {
            function.prologueInstructions.push_back(
                makeParamCopy(parameter.homeSlot,
                              MachineOperand::physicalRegister(regInfo.floatArgumentRegisters()[floatRegs],
                                                              parameter.type),
                              parameter.type));
          }
          ++floatRegs;
        } else {
          if (parameter.homeSlot >= 0) {
            function.prologueInstructions.push_back(
                makeIncomingStackParamLoad(parameter.homeSlot, parameter.type,
                                           static_cast<long long>(stackOffset)));
          }
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      } else {
        if (intRegs < regInfo.integerArgumentRegisters().size()) {
          if (parameter.homeSlot >= 0) {
            function.prologueInstructions.push_back(
                makeParamCopy(parameter.homeSlot,
                              MachineOperand::physicalRegister(regInfo.integerArgumentRegisters()[intRegs],
                                                              parameter.type),
                              parameter.type));
          }
          ++intRegs;
        } else {
          if (parameter.homeSlot >= 0) {
            function.prologueInstructions.push_back(
                makeIncomingStackParamLoad(parameter.homeSlot, parameter.type,
                                           static_cast<long long>(stackOffset)));
          }
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
    if (function.usesFramePointer) {
      function.epilogueInstructions.push_back(
          makeStackRestore(regInfo.framePointerRegister(), ValueType::int64Type(),
                           -function.savedFpOffset));
    }
    function.epilogueInstructions.push_back(
        makeStackAdjust(static_cast<long long>(function.frameSize)));
  }
}

}  // namespace compiler::backend::stack
