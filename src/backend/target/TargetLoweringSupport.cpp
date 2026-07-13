#include "../../../include/compiler/backend/target/TargetLoweringSupport.h"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace compiler::backend::target::detail {

namespace {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineGlobal;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineModule;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::ObjectType;
using compiler::backend::mir::StackObject;
using compiler::backend::mir::StackObjectKind;
using compiler::backend::mir::ValueType;

struct FunctionSignature {
  ValueType returnType;
  std::vector<ValueType> parameters;
};

struct ValueBinding {
  enum class Kind {
    Virtual,
    FrameIndex,
  };

  Kind kind = Kind::Virtual;
  ValueType type;
  int index = -1;
};

ObjectType parseObjectType(const std::string& text) {
  ObjectType type;

  std::size_t index = text.find('[');
  const std::string base = index == std::string::npos ? text : text.substr(0, index);
  if (base == "bool") {
    type.scalar = ObjectType::ScalarKind::Bool;
  } else if (base == "float") {
    type.scalar = ObjectType::ScalarKind::Float32;
  } else if (base == "long") {
    type.scalar = ObjectType::ScalarKind::Int64;
  } else if (base == "int") {
    type.scalar = ObjectType::ScalarKind::Int32;
  } else {
    type.scalar = ObjectType::ScalarKind::Void;
  }

  while (index != std::string::npos && index < text.size()) {
    const std::size_t close = text.find(']', index);
    if (close == std::string::npos) {
      break;
    }
    if (close == index + 1) {
      type.extents.push_back(std::nullopt);
    } else {
      type.extents.push_back(static_cast<std::size_t>(
          std::strtoull(text.substr(index + 1, close - index - 1).c_str(), nullptr, 10)));
    }
    index = text.find('[', close + 1);
  }

  return type;
}

ValueType scalarValueTypeFromObject(const ObjectType& objectType) {
  return ValueType::fromScalarObject(objectType);
}

ValueType parameterValueType(const ObjectType& objectType) {
  if (objectType.isArray()) {
    return ValueType::pointerTo(objectType);
  }
  return scalarValueTypeFromObject(objectType);
}

bool isFloatLiteralText(const std::string& text) {
  return text.find_first_of(".eEpP") != std::string::npos;
}

bool parseIntegerLiteral(const std::string& text, std::int64_t& value) {
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoll(text.c_str(), &end, 0);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

bool parseFloatLiteral(const std::string& text, double& value) {
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

std::string stripGlobalPrefix(const std::string& symbol) {
  if (!symbol.empty() && symbol.front() == '@') {
    return symbol.substr(1);
  }
  return symbol;
}

std::unordered_map<std::string, FunctionSignature> collectFunctionSignatures(
    const compiler::ir::core::Module& module) {
  std::unordered_map<std::string, FunctionSignature> signatures;

  for (const auto& function : module.functions()) {
    FunctionSignature signature;
    signature.returnType = scalarValueTypeFromObject(parseObjectType(function.returnType()));
    signature.parameters.reserve(function.parameters().size());
    for (const auto& parameter : function.parameters()) {
      signature.parameters.push_back(parameterValueType(parseObjectType(parameter.type)));
    }
    signatures.emplace(function.name(), std::move(signature));
  }

  auto addBuiltin = [&](std::string name, ValueType returnType, std::vector<ValueType> parameters) {
    signatures.emplace(std::move(name), FunctionSignature{std::move(returnType), std::move(parameters)});
  };

  addBuiltin("getint", ValueType::int32Type(), {});
  addBuiltin("getch", ValueType::int32Type(), {});
  addBuiltin("getfloat", ValueType::float32Type(), {});
  addBuiltin("getarray", ValueType::int32Type(), {ValueType::pointerTo(parseObjectType("int"))});
  addBuiltin("getfarray", ValueType::int32Type(), {ValueType::pointerTo(parseObjectType("float"))});
  addBuiltin("putint", ValueType::voidType(), {ValueType::int32Type()});
  addBuiltin("putch", ValueType::voidType(), {ValueType::int32Type()});
  addBuiltin("putfloat", ValueType::voidType(), {ValueType::float32Type()});
  addBuiltin("putarray", ValueType::voidType(),
             {ValueType::int32Type(), ValueType::pointerTo(parseObjectType("int"))});
  addBuiltin("putfarray", ValueType::voidType(),
             {ValueType::int32Type(), ValueType::pointerTo(parseObjectType("float"))});
  addBuiltin("starttime", ValueType::voidType(), {});
  addBuiltin("stoptime", ValueType::voidType(), {});
  addBuiltin("_sysy_starttime", ValueType::voidType(), {ValueType::int32Type()});
  addBuiltin("_sysy_stoptime", ValueType::voidType(), {ValueType::int32Type()});

  return signatures;
}

ValueType inferImmediateType(const std::string& text, const std::optional<ValueType>& hint) {
  if (hint.has_value() && !hint->isVoid()) {
    return *hint;
  }
  if (isFloatLiteralText(text)) {
    return ValueType::float32Type();
  }
  return ValueType::int32Type();
}

class FunctionLoweringContext {
 public:
  FunctionLoweringContext(const compiler::backend::target::TargetInfo& target,
                         const std::unordered_map<std::string, FunctionSignature>& signatures,
                         const std::unordered_map<std::string, ObjectType>& globalTypes,
                         const compiler::ir::core::Function& function)
      : target_(target), signatures_(signatures), globalTypes_(globalTypes), function_(function) {}

  MachineFunction run() {
    machineFunction_.name = function_.name();
    machineFunction_.returnType = scalarValueTypeFromObject(parseObjectType(function_.returnType()));
    machineFunction_.parameters.reserve(function_.parameters().size());
    for (const auto& parameter : function_.parameters()) {
      const ObjectType objectType = parseObjectType(parameter.type);
      const ValueType valueType = parameterValueType(objectType);
      bindings_[parameter.name] = ValueBinding{ValueBinding::Kind::Virtual, valueType, -1};
      machineFunction_.parameters.push_back({parameter.name, valueType, -1});
    }

    bool isEntryBlock = true;
    for (const auto& block : function_.blocks()) {
      currentBlockIsEntry_ = isEntryBlock;
      MachineBlock machineBlock;
      machineBlock.name = block.name();
      for (const auto& instruction : block.instructions()) {
        lowerInstruction(machineBlock, instruction);
      }
      machineFunction_.blocks.push_back(std::move(machineBlock));
      isEntryBlock = false;
    }

    return machineFunction_;
  }

 private:
  MachineOperand bindVirtualResult(const std::string& name, ValueType type) {
    bindings_[name] = ValueBinding{ValueBinding::Kind::Virtual, type, -1};
    return MachineOperand::virtualRegister(name, std::move(type));
  }

  int addStackObject(const std::string& name, const ObjectType& objectType, StackObjectKind kind) {
    StackObject object;
    object.kind = kind;
    object.name = name;
    object.objectType = objectType;
    object.entryAllocated = currentBlockIsEntry_;
    object.size = objectType.size(target_);
    object.alignment = objectType.alignment(target_);
    machineFunction_.stackObjects.push_back(std::move(object));
    return static_cast<int>(machineFunction_.stackObjects.size() - 1);
  }

  ValueType inferIntegerType(const std::vector<std::string>& operands) const {
    ValueType inferred = ValueType::int32Type();
    for (const auto& operand : operands) {
      if (operand.empty() || operand.front() != '%') {
        continue;
      }
      const auto found = bindings_.find(operand);
      if (found == bindings_.end()) {
        continue;
      }
      if (found->second.type.isInt64()) {
        return ValueType::int64Type();
      }
      if (found->second.type.isBool()) {
        inferred = ValueType::boolType();
      }
    }
    return inferred;
  }

  ValueType inferValueType(const std::string& operand) const {
    if (!operand.empty() && operand.front() == '%') {
      const auto found = bindings_.find(operand);
      if (found != bindings_.end()) {
        return found->second.type;
      }
    }
    if (!operand.empty() && operand.front() == '@') {
      const auto found = globalTypes_.find(stripGlobalPrefix(operand));
      if (found != globalTypes_.end()) {
        return ValueType::pointerTo(found->second);
      }
    }
    if (isFloatLiteralText(operand)) {
      return ValueType::float32Type();
    }
    return ValueType::int32Type();
  }

  MachineOperand lowerValueOperand(const std::string& operand,
                                   std::optional<ValueType> expectedType = std::nullopt) const {
    if (!operand.empty() && operand.front() == '%') {
      const auto found = bindings_.find(operand);
      if (found != bindings_.end()) {
        if (found->second.kind == ValueBinding::Kind::FrameIndex) {
          return MachineOperand::frameIndex(found->second.index, found->second.type);
        }
        return MachineOperand::virtualRegister(operand, found->second.type);
      }
      return MachineOperand::virtualRegister(
          operand, expectedType.has_value() ? *expectedType : ValueType::int32Type());
    }

    if (!operand.empty() && operand.front() == '@') {
      const std::string symbol = stripGlobalPrefix(operand);
      const auto found = globalTypes_.find(symbol);
      if (found != globalTypes_.end()) {
        return MachineOperand::globalAddress(symbol, ValueType::pointerTo(found->second));
      }
      return MachineOperand::functionSymbol(symbol);
    }

    const ValueType immediateType = inferImmediateType(operand, expectedType);
    if (immediateType.isFloat()) {
      double value = 0.0;
      parseFloatLiteral(operand, value);
      return MachineOperand::immediateFloat(value);
    }

    std::int64_t value = 0;
    parseIntegerLiteral(operand, value);
    return MachineOperand::immediateInt(value, immediateType);
  }

  void emit(MachineBlock& block, std::string opcode, std::vector<MachineOperand> operands) const {
    block.instructions.push_back(MachineInstruction::target(std::move(opcode), std::move(operands)));
  }

  void emit(MachineBlock& block, std::string opcode, MachineOperand result,
            std::vector<MachineOperand> operands) const {
    block.instructions.push_back(
        MachineInstruction::target(std::move(opcode), std::move(result), std::move(operands)));
  }

  void lowerInstruction(MachineBlock& block, const compiler::ir::core::Instruction& instruction) {
    const std::string& opcode = instruction.opcode();
    const auto& operands = instruction.operands();

    if (opcode == "alloca") {
      const ObjectType objectType = parseObjectType(operands[0]);
      const int objectIndex = addStackObject(instruction.result(), objectType, StackObjectKind::Local);
      bindings_[instruction.result()] =
          ValueBinding{ValueBinding::Kind::FrameIndex, ValueType::pointerTo(objectType), objectIndex};
      return;
    }

    if (opcode == "load") {
      const ObjectType objectType = parseObjectType(operands[0]);
      const ValueType resultType = scalarValueTypeFromObject(objectType);
      emit(block, opcode, bindVirtualResult(instruction.result(), resultType),
           {lowerValueOperand(operands[1])});
      return;
    }

    if (opcode == "phi") {
      const ObjectType objectType = parseObjectType(operands[0]);
      const ValueType resultType = scalarValueTypeFromObject(objectType);
      std::vector<compiler::backend::mir::MachinePhiIncoming> incoming;
      for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
        incoming.push_back({operands[i + 1], lowerValueOperand(operands[i], resultType)});
      }
      block.instructions.push_back(
          MachineInstruction::phi(bindVirtualResult(instruction.result(), resultType),
                                  std::move(incoming)));
      return;
    }

    if (opcode == "store") {
      const ObjectType objectType = parseObjectType(operands[0]);
      const ValueType valueType = scalarValueTypeFromObject(objectType);
      emit(block, opcode,
           {lowerValueOperand(operands[1], valueType), lowerValueOperand(operands[2])});
      return;
    }

    if (opcode == "br") {
      if (operands.size() == 1) {
        emit(block, opcode, {MachineOperand::label(operands[0])});
      } else {
        emit(block, opcode,
             {lowerValueOperand(operands[0], ValueType::boolType()),
              MachineOperand::label(operands[1]), MachineOperand::label(operands[2])});
      }
      return;
    }

    if (opcode == "ret") {
      std::vector<MachineOperand> lowered;
      if (!operands.empty()) {
        lowered.push_back(lowerValueOperand(operands[0], machineFunction_.returnType));
      }
      emit(block, opcode, std::move(lowered));
      return;
    }

    if (opcode == "call") {
      const std::string callee = stripGlobalPrefix(operands[0]);
      FunctionSignature signature;
      if (const auto found = signatures_.find(callee); found != signatures_.end()) {
        signature = found->second;
      } else {
        signature.returnType = ValueType::int32Type();
      }

      std::vector<MachineOperand> lowered;
      lowered.push_back(MachineOperand::functionSymbol(callee));
      for (std::size_t i = 1; i < operands.size(); ++i) {
        std::optional<ValueType> expectedType;
        if (i - 1 < signature.parameters.size()) {
          expectedType = signature.parameters[i - 1];
        }
        lowered.push_back(lowerValueOperand(operands[i], expectedType));
      }

      if (instruction.hasResult()) {
        emit(block, opcode, bindVirtualResult(instruction.result(), signature.returnType),
             std::move(lowered));
      } else {
        emit(block, opcode, std::move(lowered));
      }
      return;
    }

    if (opcode == "gep" || opcode == "gep.linear" || opcode == "getelementptr" ||
        opcode == "getelementptr.linear") {
      MachineOperand base = lowerValueOperand(operands[0]);
      const ValueType indexType = ValueType::int32Type();
      MachineOperand index = lowerValueOperand(operands[1], indexType);
      const bool isLinear = opcode == "gep.linear" || opcode == "getelementptr.linear";

      const std::size_t scale = isLinear
                                    ? base.type.pointee.flattenedElementType().size(target_)
                                    : (base.type.pointee.isArray()
                                           ? base.type.pointee.elementType().size(target_)
                                           : base.type.pointee.size(target_));
      ObjectType resultPointee = base.type.pointee;
      if (isLinear) {
        resultPointee = base.type.pointee.flattenedElementType();
      } else if (base.type.pointee.isArray()) {
        resultPointee = base.type.pointee.elementType();
      }

      emit(block, opcode, bindVirtualResult(instruction.result(), ValueType::pointerTo(resultPointee)),
           {std::move(base), std::move(index),
            MachineOperand::immediateInt(static_cast<std::int64_t>(scale), ValueType::int64Type())});
      return;
    }

    if (opcode == "neg" || opcode == "fneg") {
      const ValueType resultType =
          opcode == "fneg" ? ValueType::float32Type() : inferValueType(operands[0]);
      emit(block, opcode, bindVirtualResult(instruction.result(), resultType),
           {lowerValueOperand(operands[0], resultType)});
      return;
    }

    if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
        opcode == "sitofp" || opcode == "uitofp" || opcode == "fptosi") {
      const ObjectType targetObjectType = parseObjectType(operands[1]);
      const ValueType resultType = scalarValueTypeFromObject(targetObjectType);
      emit(block, opcode, bindVirtualResult(instruction.result(), resultType),
           {lowerValueOperand(operands[0])});
      return;
    }

    if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
        opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
        opcode == "or" || opcode == "xor" || opcode == "shl" || opcode == "ashr" ||
        opcode == "lshr") {
      const ValueType resultType = inferIntegerType(operands);
      emit(block, opcode, bindVirtualResult(instruction.result(), resultType),
           {lowerValueOperand(operands[0], resultType), lowerValueOperand(operands[1], resultType)});
      return;
    }

    if (opcode == "select") {
      const ValueType resultType = inferValueType(operands[1]);
      emit(block, opcode, bindVirtualResult(instruction.result(), resultType),
           {lowerValueOperand(operands[0], ValueType::boolType()),
            lowerValueOperand(operands[1], resultType),
            lowerValueOperand(operands[2], resultType)});
      return;
    }

    if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
      const ValueType resultType = ValueType::float32Type();
      emit(block, opcode, bindVirtualResult(instruction.result(), resultType),
           {lowerValueOperand(operands[0], resultType), lowerValueOperand(operands[1], resultType)});
      return;
    }

    if (opcode.rfind("icmp.", 0) == 0) {
      const ValueType compareType = inferIntegerType(operands);
      emit(block, opcode, bindVirtualResult(instruction.result(), ValueType::boolType()),
           {lowerValueOperand(operands[0], compareType), lowerValueOperand(operands[1], compareType)});
      return;
    }

    if (opcode.rfind("fcmp.", 0) == 0) {
      const ValueType compareType = ValueType::float32Type();
      emit(block, opcode, bindVirtualResult(instruction.result(), ValueType::boolType()),
           {lowerValueOperand(operands[0], compareType), lowerValueOperand(operands[1], compareType)});
    }
  }

  const compiler::backend::target::TargetInfo& target_;
  const std::unordered_map<std::string, FunctionSignature>& signatures_;
  const std::unordered_map<std::string, ObjectType>& globalTypes_;
  const compiler::ir::core::Function& function_;
  std::unordered_map<std::string, ValueBinding> bindings_;
  bool currentBlockIsEntry_ = false;
  MachineFunction machineFunction_;
};

}  // namespace

MachineModule lowerModuleDefault(const compiler::ir::core::Module& module,
                                 const compiler::backend::target::TargetInfo& target) {
  MachineModule machineModule{target, {}, {}};

  std::unordered_map<std::string, ObjectType> globalTypes;
  for (const auto& global : module.globals()) {
    const ObjectType type = parseObjectType(global.type());
    globalTypes.emplace(global.name(), type);
    machineModule.globals.push_back(
        MachineGlobal{global.name(), type, global.initializer(), global.isConst()});
  }

  const auto signatures = collectFunctionSignatures(module);
  for (const auto& function : module.functions()) {
    machineModule.functions.push_back(
        FunctionLoweringContext(target, signatures, globalTypes, function).run());
  }

  return machineModule;
}

}  // namespace compiler::backend::target::detail
