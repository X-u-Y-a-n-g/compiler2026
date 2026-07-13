#include "../../../include/compiler/ir/printer/IRPrinter.h"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::ir::printer {

namespace {

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

  [[nodiscard]] bool isArray() const { return !extents.empty(); }

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

  [[nodiscard]] bool isVoid() const { return kind == Kind::Void; }
  [[nodiscard]] bool isBool() const { return kind == Kind::Bool; }
  [[nodiscard]] bool isFloat() const { return kind == Kind::Float32; }
  [[nodiscard]] bool isInt64() const { return kind == Kind::Int64; }
  [[nodiscard]] bool isPointer() const { return kind == Kind::Pointer; }
};

struct FunctionSignature {
  ValueType returnType;
  std::vector<ValueType> parameters;
};

struct FunctionState {
  std::unordered_map<std::string, ValueType> valueTypes;
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
  switch (objectType.scalar) {
    case ObjectType::ScalarKind::Bool:
      return ValueType::boolType();
    case ObjectType::ScalarKind::Int32:
      return ValueType::int32Type();
    case ObjectType::ScalarKind::Int64:
      return ValueType::int64Type();
    case ObjectType::ScalarKind::Float32:
      return ValueType::float32Type();
    case ObjectType::ScalarKind::Void:
      return ValueType::voidType();
  }
  return ValueType::voidType();
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

std::string printScalarType(ObjectType::ScalarKind scalar) {
  switch (scalar) {
    case ObjectType::ScalarKind::Void:
      return "void";
    case ObjectType::ScalarKind::Bool:
      return "i1";
    case ObjectType::ScalarKind::Int32:
      return "i32";
    case ObjectType::ScalarKind::Int64:
      return "i64";
    case ObjectType::ScalarKind::Float32:
      return "float";
  }
  return "void";
}

std::string printObjectType(const ObjectType& type) {
  if (!type.isArray()) {
    return printScalarType(type.scalar);
  }
  const std::size_t extent = type.extents.front().value_or(0);
  return "[" + std::to_string(extent) + " x " + printObjectType(type.elementType()) + "]";
}

std::string printValueType(const ValueType& type) {
  switch (type.kind) {
    case ValueType::Kind::Void:
      return "void";
    case ValueType::Kind::Bool:
      return "i1";
    case ValueType::Kind::Int32:
      return "i32";
    case ValueType::Kind::Int64:
      return "i64";
    case ValueType::Kind::Float32:
      return "float";
    case ValueType::Kind::Pointer:
      return "ptr";
  }
  return "void";
}

bool parseIntegerLiteral(const std::string& text, long long& value);
bool parseFloatLiteral(const std::string& text, double& value);

std::string normalizeScalarLiteral(const std::string& literal, const ValueType& type) {
  long long integerValue = 0;
  if (type.isBool()) {
    if (parseIntegerLiteral(literal, integerValue)) {
      return integerValue == 0 ? "false" : "true";
    }
    return literal;
  }
  if ((type.kind == ValueType::Kind::Int32 || type.kind == ValueType::Kind::Int64) &&
      parseIntegerLiteral(literal, integerValue)) {
    return std::to_string(integerValue);
  }
  return literal;
}

std::string renderOperand(const std::string& operand, const ValueType& type) {
  if (!operand.empty() && (operand.front() == '%' || operand.front() == '@')) {
    return operand;
  }
  return normalizeScalarLiteral(operand, type);
}

std::string zeroScalarLiteral(const ObjectType& type) {
  return type.scalar == ObjectType::ScalarKind::Float32 ? "0.0" : "0";
}

bool parseIntegerLiteral(const std::string& text, long long& value) {
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

bool isAllZero(const compiler::ir::core::Constant& constant) {
  if (constant.isScalar()) {
    long long integerValue = 0;
    if (parseIntegerLiteral(constant.literal(), integerValue)) {
      return integerValue == 0;
    }
    double floatValue = 0.0;
    if (parseFloatLiteral(constant.literal(), floatValue)) {
      return floatValue == 0.0;
    }
    return false;
  }
  for (const auto& element : constant.elements()) {
    if (!isAllZero(element)) {
      return false;
    }
  }
  return true;
}

std::string printTypedConstant(const compiler::ir::core::Constant& constant, const ObjectType& type);

std::string printZeroInitializer(const ObjectType& type) {
  if (!type.isArray()) {
    return printObjectType(type) + " " + zeroScalarLiteral(type);
  }
  return printObjectType(type) + " zeroinitializer";
}

std::string printAggregateConstant(const compiler::ir::core::Constant& constant, const ObjectType& type) {
  if (isAllZero(constant)) {
    return printZeroInitializer(type);
  }

  std::ostringstream os;
  os << printObjectType(type) << " [";
  const ObjectType elementType = type.elementType();
  const std::size_t count = type.extents.front().value_or(constant.elements().size());
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      os << ", ";
    }
    if (i < constant.elements().size()) {
      os << printTypedConstant(constant.elements()[i], elementType);
    } else {
      os << printZeroInitializer(elementType);
    }
  }
  os << "]";
  return os.str();
}

std::string printTypedConstant(const compiler::ir::core::Constant& constant, const ObjectType& type) {
  if (type.isArray()) {
    return printAggregateConstant(constant, type);
  }
  return printObjectType(type) + " " +
         normalizeScalarLiteral(constant.literal(), scalarValueTypeFromObject(type));
}

FunctionSignature buildSignature(const compiler::ir::core::Function& function) {
  FunctionSignature signature;
  signature.returnType = scalarValueTypeFromObject(parseObjectType(function.returnType()));
  signature.parameters.reserve(function.parameters().size());
  for (const auto& parameter : function.parameters()) {
    signature.parameters.push_back(parameterValueType(parseObjectType(parameter.type)));
  }
  return signature;
}

std::unordered_map<std::string, FunctionSignature> collectFunctionSignatures(
    const compiler::ir::core::Module& module) {
  std::unordered_map<std::string, FunctionSignature> signatures;
  for (const auto& function : module.functions()) {
    signatures.emplace(function.name(), buildSignature(function));
  }

  auto addBuiltin = [&](std::string name, ValueType returnType, std::vector<ValueType> parameters) {
    signatures.emplace(std::move(name), FunctionSignature{std::move(returnType), std::move(parameters)});
  };

  addBuiltin("getint", ValueType::int32Type(), {});
  addBuiltin("getch", ValueType::int32Type(), {});
  addBuiltin("getfloat", ValueType::float32Type(), {});
  addBuiltin("getarray", ValueType::int32Type(), {ValueType::pointerTo(parseObjectType("int[]"))});
  addBuiltin("getfarray", ValueType::int32Type(), {ValueType::pointerTo(parseObjectType("float[]"))});
  addBuiltin("putint", ValueType::voidType(), {ValueType::int32Type()});
  addBuiltin("putch", ValueType::voidType(), {ValueType::int32Type()});
  addBuiltin("putfloat", ValueType::voidType(), {ValueType::float32Type()});
  addBuiltin("putarray", ValueType::voidType(),
             {ValueType::int32Type(), ValueType::pointerTo(parseObjectType("int[]"))});
  addBuiltin("putfarray", ValueType::voidType(),
             {ValueType::int32Type(), ValueType::pointerTo(parseObjectType("float[]"))});
  addBuiltin("starttime", ValueType::voidType(), {});
  addBuiltin("stoptime", ValueType::voidType(), {});
  addBuiltin("_sysy_starttime", ValueType::voidType(), {ValueType::int32Type()});
  addBuiltin("_sysy_stoptime", ValueType::voidType(), {ValueType::int32Type()});

  return signatures;
}

ValueType inferImmediateType(const std::string& operand, const std::optional<ValueType>& hint) {
  if (hint.has_value() && !hint->isVoid()) {
    return *hint;
  }
  if (isFloatLiteralText(operand)) {
    return ValueType::float32Type();
  }
  return ValueType::int32Type();
}

ValueType inferValueType(const std::string& operand, const FunctionState& state,
                         const std::unordered_map<std::string, ObjectType>& globals,
                         std::optional<ValueType> fallback = std::nullopt) {
  if (!operand.empty() && operand.front() == '%') {
    if (const auto found = state.valueTypes.find(operand); found != state.valueTypes.end()) {
      return found->second;
    }
  }
  if (!operand.empty() && operand.front() == '@') {
    const std::string symbol = operand.substr(1);
    if (const auto found = globals.find(symbol); found != globals.end()) {
      return ValueType::pointerTo(found->second);
    }
  }
  return inferImmediateType(operand, fallback);
}

ValueType inferIntegerType(const std::vector<std::string>& operands, const FunctionState& state,
                           const std::unordered_map<std::string, ObjectType>& globals) {
  ValueType inferred = ValueType::int32Type();
  for (const auto& operand : operands) {
    const ValueType type = inferValueType(operand, state, globals);
    if (type.isInt64()) {
      return ValueType::int64Type();
    }
    if (type.isBool()) {
      inferred = ValueType::boolType();
    }
  }
  return inferred;
}

ValueType inferInstructionResultType(const compiler::ir::core::Instruction& instruction,
                                     const FunctionState& state,
                                     const std::unordered_map<std::string, FunctionSignature>& signatures,
                                     const std::unordered_map<std::string, ObjectType>& globals) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();

  if (opcode == "alloca") {
    return ValueType::pointerTo(parseObjectType(operands[0]));
  }
  if (opcode == "load") {
    return scalarValueTypeFromObject(parseObjectType(operands[0]));
  }
  if (opcode == "phi") {
    return scalarValueTypeFromObject(parseObjectType(operands[0]));
  }
  if (opcode == "call") {
    const std::string callee = operands[0].substr(1);
    if (const auto found = signatures.find(callee); found != signatures.end()) {
      return found->second.returnType;
    }
    return ValueType::int32Type();
  }
  if (opcode == "getelementptr" || opcode == "getelementptr.linear" || opcode == "gep" ||
      opcode == "gep.linear") {
    const ValueType base = inferValueType(operands[0], state, globals);
    ObjectType pointee = base.pointee;
    if (opcode == "getelementptr.linear" || opcode == "gep.linear") {
      pointee = pointee.flattenedElementType();
    } else if (pointee.isArray()) {
      pointee = pointee.elementType();
    }
    return ValueType::pointerTo(pointee);
  }
  if (opcode == "neg") {
    return inferValueType(operands[0], state, globals);
  }
  if (opcode == "fneg" || opcode == "fadd" || opcode == "fsub" || opcode == "fmul" ||
      opcode == "fdiv" || opcode == "sitofp" || opcode == "uitofp") {
    return ValueType::float32Type();
  }
  if (opcode == "zext" || opcode == "sext" || opcode == "trunc" || opcode == "fptosi" ||
      opcode == "cast") {
    return scalarValueTypeFromObject(parseObjectType(operands[1]));
  }
  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
      opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
      opcode == "or") {
    return inferIntegerType(operands, state, globals);
  }
  if (opcode == "select") {
    return inferValueType(operands[1], state, globals);
  }
  if (opcode.rfind("icmp.", 0) == 0 || opcode.rfind("fcmp.", 0) == 0) {
    return ValueType::boolType();
  }
  return ValueType::voidType();
}

std::string formatTypedOperand(const std::string& operand, const ValueType& type) {
  return printValueType(type) + " " + renderOperand(operand, type);
}

std::string printInstruction(const compiler::ir::core::Instruction& instruction, FunctionState& state,
                             const std::unordered_map<std::string, FunctionSignature>& signatures,
                             const std::unordered_map<std::string, ObjectType>& globals) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  std::ostringstream os;

  if (instruction.hasResult()) {
    const ValueType resultType = inferInstructionResultType(instruction, state, signatures, globals);
    state.valueTypes[instruction.result()] = resultType;
    os << instruction.result() << " = ";
  }

  if (opcode == "alloca") {
    os << "alloca " << printObjectType(parseObjectType(operands[0]));
    return os.str();
  }

  if (opcode == "load") {
    const ObjectType objectType = parseObjectType(operands[0]);
    os << "load " << printObjectType(objectType) << ", ptr " << operands[1];
    return os.str();
  }

  if (opcode == "phi") {
    const ValueType valueType = scalarValueTypeFromObject(parseObjectType(operands[0]));
    os << "phi " << printValueType(valueType) << " ";
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      if (i != 1) {
        os << ", ";
      }
      os << "[" << renderOperand(operands[i], valueType) << ", %" << operands[i + 1] << "]";
    }
    return os.str();
  }

  if (opcode == "store") {
    const ValueType valueType = scalarValueTypeFromObject(parseObjectType(operands[0]));
    os << "store " << formatTypedOperand(operands[1], valueType) << ", ptr " << operands[2];
    return os.str();
  }

  if (opcode == "br") {
    if (operands.size() == 1) {
      os << "br label %" << operands[0];
    } else {
      os << "br " << formatTypedOperand(operands[0], ValueType::boolType()) << ", label %"
         << operands[1] << ", label %" << operands[2];
    }
    return os.str();
  }

  if (opcode == "ret") {
    if (operands.empty()) {
      os << "ret void";
    } else {
      const ValueType valueType = inferValueType(operands[0], state, globals);
      os << "ret " << formatTypedOperand(operands[0], valueType);
    }
    return os.str();
  }

  if (opcode == "call") {
    const std::string callee = operands[0].substr(1);
    FunctionSignature signature;
    if (const auto found = signatures.find(callee); found != signatures.end()) {
      signature = found->second;
    } else {
      signature.returnType = ValueType::int32Type();
    }

    os << "call " << printValueType(signature.returnType) << " @" << callee << "(";
    for (std::size_t i = 1; i < operands.size(); ++i) {
      if (i != 1) {
        os << ", ";
      }
      ValueType valueType = inferValueType(operands[i], state, globals);
      if (i - 1 < signature.parameters.size()) {
        valueType = signature.parameters[i - 1];
      }
      os << formatTypedOperand(operands[i], valueType);
    }
    os << ")";
    return os.str();
  }

  if (opcode == "getelementptr" || opcode == "getelementptr.linear" || opcode == "gep" ||
      opcode == "gep.linear") {
    const ValueType baseType = inferValueType(operands[0], state, globals);
    const bool isLinear = opcode == "getelementptr.linear" || opcode == "gep.linear";
    const ObjectType baseObject = baseType.pointee;
    const ObjectType elementType =
        isLinear ? baseObject.flattenedElementType()
                 : (baseObject.isArray() ? baseObject.elementType() : baseObject);

    if (!isLinear && baseObject.isArray()) {
      os << "getelementptr inbounds " << printObjectType(baseObject) << ", ptr " << operands[0]
         << ", i32 0, " << formatTypedOperand(operands[1], ValueType::int32Type());
    } else {
      os << "getelementptr inbounds " << printObjectType(elementType) << ", ptr " << operands[0]
         << ", " << formatTypedOperand(operands[1], ValueType::int32Type());
    }
    return os.str();
  }

  if (opcode == "neg") {
    const ValueType valueType = inferValueType(operands[0], state, globals);
    os << "sub " << printValueType(valueType) << " 0, " << renderOperand(operands[0], valueType);
    return os.str();
  }

  if (opcode == "fneg") {
    os << "fneg float " << operands[0];
    return os.str();
  }

  if (opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    const ValueType sourceType = inferValueType(operands[0], state, globals);
    const ValueType targetType = scalarValueTypeFromObject(parseObjectType(operands[1]));
    os << opcode << " " << formatTypedOperand(operands[0], sourceType) << " to "
       << printValueType(targetType);
    return os.str();
  }

  if (opcode == "sitofp" || opcode == "uitofp" || opcode == "fptosi" || opcode == "cast") {
    const ValueType sourceType = inferValueType(operands[0], state, globals);
    const ValueType targetType = scalarValueTypeFromObject(parseObjectType(operands[1]));
    os << opcode << " " << formatTypedOperand(operands[0], sourceType) << " to "
       << printValueType(targetType);
    return os.str();
  }

  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" ||
      opcode == "mod" || opcode == "sdiv" || opcode == "srem" || opcode == "and" ||
      opcode == "or") {
    const ValueType valueType = inferIntegerType(operands, state, globals);
    os << opcode << " " << printValueType(valueType) << " "
       << renderOperand(operands[0], valueType) << ", " << renderOperand(operands[1], valueType);
    return os.str();
  }

  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    os << opcode << " float " << operands[0] << ", " << operands[1];
    return os.str();
  }

  if (opcode == "select") {
    const ValueType valueType = inferValueType(operands[1], state, globals);
    os << "select i1 " << renderOperand(operands[0], ValueType::boolType()) << ", "
       << formatTypedOperand(operands[1], valueType) << ", "
       << formatTypedOperand(operands[2], valueType);
    return os.str();
  }

  if (opcode.rfind("icmp.", 0) == 0) {
    const std::string predicate = opcode.substr(5);
    const ValueType valueType = inferIntegerType(operands, state, globals);
    os << "icmp " << predicate << " " << printValueType(valueType) << " "
       << renderOperand(operands[0], valueType) << ", " << renderOperand(operands[1], valueType);
    return os.str();
  }

  if (opcode.rfind("fcmp.", 0) == 0) {
    const std::string predicate = opcode.substr(5);
    os << "fcmp " << predicate << " float " << operands[0] << ", " << operands[1];
    return os.str();
  }

  os << "; unsupported opcode " << opcode;
  return os.str();
}

std::unordered_map<std::string, ObjectType> collectGlobalTypes(const compiler::ir::core::Module& module) {
  std::unordered_map<std::string, ObjectType> globals;
  for (const auto& global : module.globals()) {
    globals.emplace(global.name(), parseObjectType(global.type()));
  }
  return globals;
}

std::vector<std::string> collectExternalDeclarations(
    const compiler::ir::core::Module& module,
    const std::unordered_map<std::string, FunctionSignature>& signatures) {
  std::unordered_set<std::string> defined;
  for (const auto& function : module.functions()) {
    defined.insert(function.name());
  }

  std::unordered_set<std::string> referenced;
  for (const auto& function : module.functions()) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() == "call" && !instruction.operands().empty() &&
            !instruction.operands()[0].empty() && instruction.operands()[0].front() == '@') {
          const std::string callee = instruction.operands()[0].substr(1);
          if (defined.find(callee) == defined.end()) {
            referenced.insert(callee);
          }
        }
      }
    }
  }

  std::vector<std::string> declarations;
  declarations.reserve(referenced.size());
  for (const auto& callee : referenced) {
    auto found = signatures.find(callee);
    if (found == signatures.end()) {
      declarations.push_back("declare i32 @" + callee + "(...)");
      continue;
    }

    std::ostringstream os;
    os << "declare " << printValueType(found->second.returnType) << " @" << callee << "(";
    for (std::size_t i = 0; i < found->second.parameters.size(); ++i) {
      if (i != 0) {
        os << ", ";
      }
      os << printValueType(found->second.parameters[i]);
    }
    os << ")";
    declarations.push_back(os.str());
  }
  return declarations;
}

}  // namespace

std::string IRPrinter::print(const compiler::ir::core::Module& module) const {
  std::ostringstream os;
  os << "; ModuleID = '" << module.sourceName() << "'\n";
  os << "source_filename = \"" << module.sourceName() << "\"\n\n";

  const auto signatures = collectFunctionSignatures(module);
  const auto globalTypes = collectGlobalTypes(module);

  for (const auto& global : module.globals()) {
    const ObjectType type = parseObjectType(global.type());
    os << "@" << global.name() << " = " << (global.isConst() ? "constant " : "global ")
       << printTypedConstant(global.initializer(), type) << '\n';
  }
  if (!module.globals().empty()) {
    os << '\n';
  }

  const auto declarations = collectExternalDeclarations(module, signatures);
  for (const auto& declaration : declarations) {
    os << declaration << '\n';
  }
  if (!declarations.empty()) {
    os << '\n';
  }

  for (const auto& note : module.passLog()) {
    os << "; pass: " << note << '\n';
  }
  if (!module.passLog().empty()) {
    os << '\n';
  }

  for (const auto& function : module.functions()) {
    const auto signature = signatures.at(function.name());
    os << "define " << printValueType(signature.returnType) << " @" << function.name() << "(";
    for (std::size_t i = 0; i < function.parameters().size(); ++i) {
      if (i != 0) {
        os << ", ";
      }
      os << printValueType(signature.parameters[i]) << " " << function.parameters()[i].name;
    }
    os << ") {\n";

    FunctionState state;
    for (std::size_t i = 0; i < function.parameters().size(); ++i) {
      state.valueTypes.emplace(function.parameters()[i].name, signature.parameters[i]);
    }

    for (const auto& block : function.blocks()) {
      os << block.name() << ":\n";
      for (const auto& instruction : block.instructions()) {
        os << "  " << printInstruction(instruction, state, signatures, globalTypes) << '\n';
      }
    }
    os << "}\n\n";
  }

  return os.str();
}

}  // namespace compiler::ir::printer
