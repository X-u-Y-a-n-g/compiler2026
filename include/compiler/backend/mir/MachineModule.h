#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TargetInfo.h"
#include "Constant.h"

namespace compiler::backend::mir {

struct ObjectType {
  enum class ScalarKind {
    Void,
    Bool,
    Int32,
    Int64,
    Float32,
  };

  ScalarKind scalar = ScalarKind::Void;
  std::vector<std::optional<std::size_t>> extents;

  [[nodiscard]] bool isVoid() const { return scalar == ScalarKind::Void && extents.empty(); }
  [[nodiscard]] bool isArray() const { return !extents.empty(); }
  [[nodiscard]] bool isBoolScalar() const { return !isArray() && scalar == ScalarKind::Bool; }
  [[nodiscard]] bool isInt32Scalar() const { return !isArray() && scalar == ScalarKind::Int32; }
  [[nodiscard]] bool isInt64Scalar() const { return !isArray() && scalar == ScalarKind::Int64; }
  [[nodiscard]] bool isFloatScalar() const { return !isArray() && scalar == ScalarKind::Float32; }
  [[nodiscard]] bool isIntegerScalar() const {
    return !isArray() &&
           (scalar == ScalarKind::Bool || scalar == ScalarKind::Int32 ||
            scalar == ScalarKind::Int64);
  }

  [[nodiscard]] ObjectType elementType() const {
    if (!isArray()) {
      return *this;
    }
    ObjectType element = *this;
    element.extents.erase(element.extents.begin());
    return element;
  }

  [[nodiscard]] ObjectType flattenedElementType() const {
    ObjectType element = *this;
    while (element.isArray()) {
      element = element.elementType();
    }
    return element;
  }

  [[nodiscard]] std::size_t scalarSize(const compiler::backend::target::TargetInfo&) const {
    switch (scalar) {
      case ScalarKind::Bool:
      case ScalarKind::Int32:
      case ScalarKind::Float32:
        return 4;
      case ScalarKind::Int64:
        return 8;
      case ScalarKind::Void:
        return 0;
    }
    return 0;
  }

  [[nodiscard]] std::size_t scalarAlignment(
      const compiler::backend::target::TargetInfo& target) const {
    const std::size_t size = scalarSize(target);
    return size == 0 ? 1 : size;
  }

  [[nodiscard]] std::size_t size(const compiler::backend::target::TargetInfo& target) const {
    std::size_t result = scalarSize(target);
    if (isArray()) {
      for (const auto& extent : extents) {
        if (!extent.has_value()) {
          return 0;
        }
        result *= *extent;
      }
    }
    return result;
  }

  [[nodiscard]] std::size_t alignment(
      const compiler::backend::target::TargetInfo& target) const {
    if (!isArray()) {
      return scalarAlignment(target);
    }
    return elementType().alignment(target);
  }

  [[nodiscard]] std::string toString() const {
    std::string text;
    switch (scalar) {
      case ScalarKind::Void:
        text = "void";
        break;
      case ScalarKind::Bool:
        text = "bool";
        break;
      case ScalarKind::Int32:
        text = "int";
        break;
      case ScalarKind::Int64:
        text = "long";
        break;
      case ScalarKind::Float32:
        text = "float";
        break;
    }
    for (const auto& extent : extents) {
      if (extent.has_value()) {
        text += "[" + std::to_string(*extent) + "]";
      } else {
        text += "[]";
      }
    }
    return text;
  }
};

struct ValueType {
  enum class Kind {
    Void,
    Bool,
    Int32,
    Int64,
    Float32,
    Pointer,
  };

  Kind kind = Kind::Void;
  ObjectType pointee;

  static ValueType voidType() { return {}; }
  static ValueType boolType() { return ValueType{Kind::Bool, {}}; }
  static ValueType int32Type() { return ValueType{Kind::Int32, {}}; }
  static ValueType int64Type() { return ValueType{Kind::Int64, {}}; }
  static ValueType float32Type() { return ValueType{Kind::Float32, {}}; }
  static ValueType pointerTo(ObjectType pointeeType) {
    return ValueType{Kind::Pointer, std::move(pointeeType)};
  }

  static ValueType fromScalarObject(const ObjectType& objectType) {
    switch (objectType.scalar) {
      case ObjectType::ScalarKind::Bool:
        return boolType();
      case ObjectType::ScalarKind::Int32:
        return int32Type();
      case ObjectType::ScalarKind::Int64:
        return int64Type();
      case ObjectType::ScalarKind::Float32:
        return float32Type();
      case ObjectType::ScalarKind::Void:
        return voidType();
    }
    return voidType();
  }

  [[nodiscard]] bool isVoid() const { return kind == Kind::Void; }
  [[nodiscard]] bool isBool() const { return kind == Kind::Bool; }
  [[nodiscard]] bool isInt32() const { return kind == Kind::Int32; }
  [[nodiscard]] bool isInt64() const { return kind == Kind::Int64; }
  [[nodiscard]] bool isFloat() const { return kind == Kind::Float32; }
  [[nodiscard]] bool isPointer() const { return kind == Kind::Pointer; }
  [[nodiscard]] bool isIntegerLike() const {
    return kind == Kind::Bool || kind == Kind::Int32 || kind == Kind::Int64;
  }

  [[nodiscard]] std::size_t size(const compiler::backend::target::TargetInfo& target) const {
    switch (kind) {
      case Kind::Bool:
      case Kind::Int32:
      case Kind::Float32:
        return 4;
      case Kind::Int64:
        return 8;
      case Kind::Pointer:
        return target.pointerSize();
      case Kind::Void:
        return 0;
    }
    return 0;
  }

  [[nodiscard]] std::size_t alignment(
      const compiler::backend::target::TargetInfo& target) const {
    const std::size_t sizeValue = size(target);
    return sizeValue == 0 ? 1 : sizeValue;
  }

  [[nodiscard]] std::string toString() const {
    switch (kind) {
      case Kind::Void:
        return "void";
      case Kind::Bool:
        return "bool";
      case Kind::Int32:
        return "int";
      case Kind::Int64:
        return "long";
      case Kind::Float32:
        return "float";
      case Kind::Pointer:
        return pointee.toString() + "*";
    }
    return "void";
  }
};

enum class MachineOperandKind {
  None,
  ImmediateInt,
  ImmediateFloat,
  VirtualRegister,
  PhysicalRegister,
  FrameIndex,
  StackSlot,
  StackObject,
  GlobalAddress,
  FunctionSymbol,
  Label,
};

struct MachineOperand {
  MachineOperandKind kind = MachineOperandKind::None;
  ValueType type;
  std::string text;
  std::int64_t intValue = 0;
  double floatValue = 0.0;
  int index = -1;

  static MachineOperand immediateInt(std::int64_t value, ValueType valueType = ValueType::int32Type()) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::ImmediateInt;
    operand.type = valueType;
    operand.intValue = value;
    return operand;
  }

  static MachineOperand immediateFloat(double value) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::ImmediateFloat;
    operand.type = ValueType::float32Type();
    operand.floatValue = value;
    return operand;
  }

  static MachineOperand virtualRegister(std::string name, ValueType valueType) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::VirtualRegister;
    operand.type = std::move(valueType);
    operand.text = std::move(name);
    return operand;
  }

  static MachineOperand physicalRegister(std::string name, ValueType valueType) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::PhysicalRegister;
    operand.type = std::move(valueType);
    operand.text = std::move(name);
    return operand;
  }

  static MachineOperand frameIndex(int objectIndex, ValueType valueType) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::FrameIndex;
    operand.type = std::move(valueType);
    operand.index = objectIndex;
    return operand;
  }

  static MachineOperand stackSlot(int slotIndex, ValueType valueType) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::StackSlot;
    operand.type = std::move(valueType);
    operand.index = slotIndex;
    return operand;
  }

  static MachineOperand stackObject(int objectIndex, ValueType valueType) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::StackObject;
    operand.type = std::move(valueType);
    operand.index = objectIndex;
    return operand;
  }

  static MachineOperand globalAddress(std::string symbol, ValueType valueType) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::GlobalAddress;
    operand.type = std::move(valueType);
    operand.text = std::move(symbol);
    return operand;
  }

  static MachineOperand functionSymbol(std::string symbol) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::FunctionSymbol;
    operand.text = std::move(symbol);
    return operand;
  }

  static MachineOperand label(std::string name) {
    MachineOperand operand;
    operand.kind = MachineOperandKind::Label;
    operand.text = std::move(name);
    return operand;
  }

  [[nodiscard]] bool isValid() const { return kind != MachineOperandKind::None; }
  [[nodiscard]] bool isRegister() const {
    return kind == MachineOperandKind::VirtualRegister ||
           kind == MachineOperandKind::PhysicalRegister;
  }
};

enum class MachineInstructionKind {
  Target,
  Copy,
  Phi,
  SpillReload,
  SpillStore,
  FrameLoad,
  FrameStore,
  StackAdjust,
  StackSetFramePointer,
  StackSave,
  StackRestore,
  StackParameterLoad,
};

struct MachinePhiIncoming {
  std::string block;
  MachineOperand value;
};

struct MachineInstruction {
  MachineInstructionKind kind = MachineInstructionKind::Target;
  std::string opcode;
  bool hasResult = false;
  MachineOperand result;
  std::vector<MachineOperand> operands;
  std::vector<MachinePhiIncoming> phiInputs;

  static MachineInstruction target(std::string opcodeName, std::vector<MachineOperand> operands = {}) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::Target;
    instruction.opcode = std::move(opcodeName);
    instruction.operands = std::move(operands);
    return instruction;
  }

  static MachineInstruction target(std::string opcodeName, MachineOperand resultOperand,
                                   std::vector<MachineOperand> operands = {}) {
    MachineInstruction instruction = target(std::move(opcodeName), std::move(operands));
    instruction.hasResult = true;
    instruction.result = std::move(resultOperand);
    return instruction;
  }

  static MachineInstruction copy(MachineOperand resultOperand, MachineOperand source) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::Copy;
    instruction.opcode = "copy";
    instruction.hasResult = true;
    instruction.result = std::move(resultOperand);
    instruction.operands.push_back(std::move(source));
    return instruction;
  }

  static MachineInstruction phi(MachineOperand resultOperand,
                                std::vector<MachinePhiIncoming> incoming) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::Phi;
    instruction.opcode = "phi";
    instruction.hasResult = true;
    instruction.result = std::move(resultOperand);
    instruction.phiInputs = std::move(incoming);
    return instruction;
  }

  static MachineInstruction spillReload(MachineOperand resultOperand, MachineOperand frameOperand) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::SpillReload;
    instruction.opcode = "spill.reload";
    instruction.hasResult = true;
    instruction.result = std::move(resultOperand);
    instruction.operands.push_back(std::move(frameOperand));
    return instruction;
  }

  static MachineInstruction spillStore(MachineOperand source, MachineOperand frameOperand) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::SpillStore;
    instruction.opcode = "spill.store";
    instruction.operands.push_back(std::move(source));
    instruction.operands.push_back(std::move(frameOperand));
    return instruction;
  }

  static MachineInstruction frameLoad(MachineOperand resultOperand, MachineOperand frameOperand) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::FrameLoad;
    instruction.opcode = "frame.load";
    instruction.hasResult = true;
    instruction.result = std::move(resultOperand);
    instruction.operands.push_back(std::move(frameOperand));
    return instruction;
  }

  static MachineInstruction frameStore(MachineOperand source, MachineOperand frameOperand) {
    MachineInstruction instruction;
    instruction.kind = MachineInstructionKind::FrameStore;
    instruction.opcode = "frame.store";
    instruction.operands.push_back(std::move(source));
    instruction.operands.push_back(std::move(frameOperand));
    return instruction;
  }
};

struct MachineBlock {
  std::string name;
  std::vector<MachineInstruction> instructions;
};

struct MachineParameter {
  std::string name;
  ValueType type;
  int homeSlot = -1;
};

enum class StackObjectKind {
  Local,
  Spill,
};

struct StackObject {
  StackObjectKind kind = StackObjectKind::Local;
  std::string name;
  ObjectType objectType;
  bool entryAllocated = false;
  std::size_t size = 0;
  std::size_t alignment = 1;
  int offset = 0;
};

struct MachineFunction {
  std::string name;
  ValueType returnType;
  std::vector<MachineParameter> parameters;
  std::vector<MachineBlock> blocks;
  std::vector<MachineInstruction> prologueInstructions;
  std::vector<MachineInstruction> epilogueInstructions;
  std::vector<StackObject> stackObjects;
  std::vector<std::string> usedIntegerCalleeSavedRegisters;
  std::vector<std::string> usedFloatCalleeSavedRegisters;
  std::vector<int> savedIntegerRegisterOffsets;
  std::vector<int> savedFloatRegisterOffsets;
  std::size_t frameSize = 0;
  std::size_t outgoingArgSize = 0;
  bool usesFramePointer = true;
  int savedRaOffset = 0;
  int savedFpOffset = 0;
};

struct MachineGlobal {
  std::string name;
  ObjectType type;
  compiler::ir::core::Constant initializer;
  bool isConst = false;
};

struct MachineModule {
  compiler::backend::target::TargetInfo target;
  std::vector<MachineGlobal> globals;
  std::vector<MachineFunction> functions;
};

}  // namespace compiler::backend::mir
