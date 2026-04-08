#include "compiler/frontend/types/TypeChecker.h"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace compiler::frontend::types {

namespace {

using compiler::frontend::ast::BinaryExpr;
using compiler::frontend::ast::BlockStmt;
using compiler::frontend::ast::CallExpr;
using compiler::frontend::ast::CaseStmt;
using compiler::frontend::ast::Decl;
using compiler::frontend::ast::DeclStmt;
using compiler::frontend::ast::DefaultStmt;
using compiler::frontend::ast::DoWhileStmt;
using compiler::frontend::ast::Expr;
using compiler::frontend::ast::ExprStmt;
using compiler::frontend::ast::FloatLiteralExpr;
using compiler::frontend::ast::ForStmt;
using compiler::frontend::ast::FunctionDecl;
using compiler::frontend::ast::IdentifierExpr;
using compiler::frontend::ast::IfStmt;
using compiler::frontend::ast::IndexExpr;
using compiler::frontend::ast::InitListExpr;
using compiler::frontend::ast::IntegerLiteral;
using compiler::frontend::ast::ReturnStmt;
using compiler::frontend::ast::Stmt;
using compiler::frontend::ast::StringLiteralExpr;
using compiler::frontend::ast::SwitchStmt;
using compiler::frontend::ast::TranslationUnit;
using compiler::frontend::ast::UnaryExpr;
using compiler::frontend::ast::VariableDecl;
using compiler::frontend::ast::VariableDeclGroup;
using compiler::frontend::ast::WhileStmt;
using compiler::frontend::lexer::TokenKind;
using compiler::frontend::lexer::toString;
using compiler::support::DiagnosticEngine;
using compiler::support::SourceLocation;

constexpr std::size_t kIgnoreFirstArrayDimension = 1;

enum class SymbolKind {
  Variable,
  Function,
};

struct CheckedType {
  TypeKind kind = TypeKind::Unknown;
  std::size_t arrayRank = 0;
  std::vector<std::optional<long long>> arrayDimensions;
  bool isConst = false;

  [[nodiscard]] bool isUnknown() const { return kind == TypeKind::Unknown; }
  [[nodiscard]] bool isVoid() const { return arrayRank == 0 && kind == TypeKind::Void; }
  [[nodiscard]] bool isArray() const { return arrayRank > 0; }

  [[nodiscard]] std::string toString() const {
    std::string text = Type{kind}.toString();
    for (std::size_t i = 0; i < arrayRank; ++i) {
      text += "[]";
    }
    return text;
  }
};

struct FunctionInfo {
  SourceLocation location;
  CheckedType returnType;
  std::vector<CheckedType> parameters;
};

struct SymbolInfo {
  SymbolKind kind = SymbolKind::Variable;
  SourceLocation location;
  CheckedType type;
  std::optional<long long> integerConstantValue;
};

class Checker {
 public:
  Checker(const TranslationUnit& unit, DiagnosticEngine& diagnostics)
      : unit_(unit), diagnostics_(diagnostics) {}

  bool run() {
    pushScope();
    registerBuiltins();
    collectGlobalDeclarations();
    analyzeGlobalDeclarations();
    return !diagnostics_.hasErrors();
  }

 private:
  void pushScope() { scopes_.emplace_back(); }

  void popScope() {
    if (!scopes_.empty()) {
      scopes_.pop_back();
    }
  }

  static CheckedType scalarType(TypeKind kind, bool isConst = false) {
    return CheckedType{kind, 0, {}, isConst};
  }

  static CheckedType arrayType(TypeKind kind, std::vector<std::optional<long long>> dimensions,
                               bool isConst = false) {
    return CheckedType{kind, dimensions.size(), std::move(dimensions), isConst};
  }

  static bool isIntegralScalar(const CheckedType& type) {
    return !type.isArray() &&
           (type.kind == TypeKind::Int || type.kind == TypeKind::Long ||
            type.kind == TypeKind::Bool);
  }

  static bool isNumericScalar(const CheckedType& type) {
    return !type.isArray() &&
           (type.kind == TypeKind::Int || type.kind == TypeKind::Long ||
            type.kind == TypeKind::Bool || type.kind == TypeKind::Float);
  }

  static bool parseIntegerLiteral(const std::string& text, long long& value) {
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoll(text.c_str(), &end, 0);
    if (errno == ERANGE || end == nullptr || *end != '\0') {
      return false;
    }
    value = parsed;
    return true;
  }

  static std::optional<long long> tryEvaluateSyntacticIntegerConstant(const Expr& expression) {
    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      long long value = 0;
      if (parseIntegerLiteral(integer->value, value)) {
        return value;
      }
      return std::nullopt;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      const auto operand = tryEvaluateSyntacticIntegerConstant(*unary->operand);
      if (!operand.has_value()) {
        return std::nullopt;
      }
      switch (unary->op) {
        case TokenKind::Plus:
          return operand;
        case TokenKind::Minus:
          if (*operand == std::numeric_limits<long long>::min()) {
            return std::nullopt;
          }
          return -*operand;
        case TokenKind::LogicalNot:
          return *operand == 0 ? 1 : 0;
        default:
          return std::nullopt;
      }
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Assign || binary->op == TokenKind::Comma) {
        return std::nullopt;
      }

      const auto lhs = tryEvaluateSyntacticIntegerConstant(*binary->lhs);
      const auto rhs = tryEvaluateSyntacticIntegerConstant(*binary->rhs);
      if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
      }

      switch (binary->op) {
        case TokenKind::Plus:
          return *lhs + *rhs;
        case TokenKind::Minus:
          return *lhs - *rhs;
        case TokenKind::Star:
          return *lhs * *rhs;
        case TokenKind::Slash:
          if (*rhs == 0) {
            return std::nullopt;
          }
          return *lhs / *rhs;
        case TokenKind::Percent:
          if (*rhs == 0) {
            return std::nullopt;
          }
          return *lhs % *rhs;
        case TokenKind::Less:
          return *lhs < *rhs ? 1 : 0;
        case TokenKind::LessEqual:
          return *lhs <= *rhs ? 1 : 0;
        case TokenKind::Greater:
          return *lhs > *rhs ? 1 : 0;
        case TokenKind::GreaterEqual:
          return *lhs >= *rhs ? 1 : 0;
        case TokenKind::EqualEqual:
          return *lhs == *rhs ? 1 : 0;
        case TokenKind::NotEqual:
          return *lhs != *rhs ? 1 : 0;
        case TokenKind::LogicalAnd:
          return (*lhs != 0 && *rhs != 0) ? 1 : 0;
        case TokenKind::LogicalOr:
          return (*lhs != 0 || *rhs != 0) ? 1 : 0;
        case TokenKind::Ampersand:
          return *lhs & *rhs;
        case TokenKind::Pipe:
          return *lhs | *rhs;
        default:
          return std::nullopt;
      }
    }

    return std::nullopt;
  }

  static CheckedType promoteArithmetic(const CheckedType& type) {
    if (type.kind == TypeKind::Bool) {
      return scalarType(TypeKind::Int);
    }
    return scalarType(type.kind);
  }

  static CheckedType commonArithmeticType(const CheckedType& lhs, const CheckedType& rhs) {
    if (lhs.kind == TypeKind::Float || rhs.kind == TypeKind::Float) {
      return scalarType(TypeKind::Float);
    }
    if (lhs.kind == TypeKind::Long || rhs.kind == TypeKind::Long) {
      return scalarType(TypeKind::Long);
    }
    return scalarType(TypeKind::Int);
  }

  static bool canImplicitlyConvert(const CheckedType& from, const CheckedType& to) {
    if (from.isUnknown() || to.isUnknown()) {
      return true;
    }
    if (from.isArray() || to.isArray()) {
      return false;
    }
    if (from.kind == TypeKind::Void || to.kind == TypeKind::Void) {
      return false;
    }
    if (isNumericScalar(from) && isNumericScalar(to)) {
      return true;
    }
    return from.kind == to.kind;
  }

  static bool haveCompatibleArrayShape(const CheckedType& argument,
                                       const CheckedType& parameter) {
    if (!argument.isArray() || !parameter.isArray()) {
      return false;
    }
    if (argument.kind != parameter.kind || argument.arrayRank < parameter.arrayRank) {
      return false;
    }

    if (parameter.arrayRank <= 1) {
      return true;
    }

    const std::size_t trailingDims = parameter.arrayRank - kIgnoreFirstArrayDimension;
    const std::size_t argumentStart = argument.arrayDimensions.size() - trailingDims;
    for (std::size_t i = 0; i < trailingDims; ++i) {
      const auto& argumentDimension = argument.arrayDimensions[argumentStart + i];
      const auto& parameterDimension = parameter.arrayDimensions[kIgnoreFirstArrayDimension + i];
      if (argumentDimension.has_value() && parameterDimension.has_value() &&
          argumentDimension != parameterDimension) {
        return false;
      }
    }
    return true;
  }

  static bool canPassArgument(const CheckedType& argument, const CheckedType& parameter) {
    if (argument.isUnknown() || parameter.isUnknown()) {
      return true;
    }
    if (parameter.isArray()) {
      return haveCompatibleArrayShape(argument, parameter);
    }
    if (argument.isArray()) {
      return false;
    }
    return canImplicitlyConvert(argument, parameter);
  }

  static std::optional<long long> totalArrayCapacity(const CheckedType& type) {
    if (!type.isArray()) {
      return std::nullopt;
    }

    long long capacity = 1;
    for (const auto& dimension : type.arrayDimensions) {
      if (!dimension.has_value() || *dimension <= 0) {
        return std::nullopt;
      }
      if (capacity > std::numeric_limits<long long>::max() / *dimension) {
        return std::nullopt;
      }
      capacity *= *dimension;
    }
    return capacity;
  }

  static CheckedType elementTypeOf(const CheckedType& arrayType) {
    return scalarType(arrayType.kind, arrayType.isConst);
  }

  bool declareInCurrentScope(const std::string& name, SymbolKind kind,
                             const SourceLocation& location, const CheckedType& type,
                             std::optional<long long> integerConstantValue = std::nullopt) {
    if (name.empty()) {
      return false;
    }
    auto& scope = scopes_.back();
    if (scope.find(name) != scope.end()) {
      return false;
    }
    scope.emplace(name, SymbolInfo{kind, location, type, integerConstantValue});
    return true;
  }

  const SymbolInfo* lookupSymbol(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  SymbolInfo* lookupSymbolMutable(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  const FunctionInfo* lookupFunction(const std::string& name) const {
    const auto it = functions_.find(name);
    if (it == functions_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  std::optional<long long> tryEvaluateIntegerConstant(const Expr& expression) const {
    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      long long value = 0;
      if (parseIntegerLiteral(integer->value, value)) {
        return value;
      }
      return std::nullopt;
    }

    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* symbol = lookupSymbol(identifier->name);
      if (symbol == nullptr || symbol->kind != SymbolKind::Variable || !symbol->type.isConst ||
          !symbol->integerConstantValue.has_value()) {
        return std::nullopt;
      }
      return symbol->integerConstantValue;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      const auto operand = tryEvaluateIntegerConstant(*unary->operand);
      if (!operand.has_value()) {
        return std::nullopt;
      }
      switch (unary->op) {
        case TokenKind::Plus:
          return operand;
        case TokenKind::Minus:
          if (*operand == std::numeric_limits<long long>::min()) {
            return std::nullopt;
          }
          return -*operand;
        case TokenKind::LogicalNot:
          return *operand == 0 ? 1 : 0;
        default:
          return std::nullopt;
      }
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Assign || binary->op == TokenKind::Comma) {
        return std::nullopt;
      }

      const auto lhs = tryEvaluateIntegerConstant(*binary->lhs);
      const auto rhs = tryEvaluateIntegerConstant(*binary->rhs);
      if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
      }

      switch (binary->op) {
        case TokenKind::Plus:
          return *lhs + *rhs;
        case TokenKind::Minus:
          return *lhs - *rhs;
        case TokenKind::Star:
          return *lhs * *rhs;
        case TokenKind::Slash:
          if (*rhs == 0) {
            return std::nullopt;
          }
          return *lhs / *rhs;
        case TokenKind::Percent:
          if (*rhs == 0) {
            return std::nullopt;
          }
          return *lhs % *rhs;
        case TokenKind::Less:
          return *lhs < *rhs ? 1 : 0;
        case TokenKind::LessEqual:
          return *lhs <= *rhs ? 1 : 0;
        case TokenKind::Greater:
          return *lhs > *rhs ? 1 : 0;
        case TokenKind::GreaterEqual:
          return *lhs >= *rhs ? 1 : 0;
        case TokenKind::EqualEqual:
          return *lhs == *rhs ? 1 : 0;
        case TokenKind::NotEqual:
          return *lhs != *rhs ? 1 : 0;
        case TokenKind::LogicalAnd:
          return (*lhs != 0 && *rhs != 0) ? 1 : 0;
        case TokenKind::LogicalOr:
          return (*lhs != 0 || *rhs != 0) ? 1 : 0;
        case TokenKind::Ampersand:
          return *lhs & *rhs;
        case TokenKind::Pipe:
          return *lhs | *rhs;
        default:
          return std::nullopt;
      }
    }

    return std::nullopt;
  }

  std::optional<long long> tryEvaluateScalarIntegerInitializer(const Expr& initializer) const {
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&initializer)) {
      if (initList->elements.size() != 1) {
        return std::nullopt;
      }
      return tryEvaluateScalarIntegerInitializer(*initList->elements.front());
    }
    return tryEvaluateIntegerConstant(initializer);
  }

  std::vector<std::optional<long long>> buildSyntacticArrayDimensions(
      const VariableDecl& variable) const {
    std::vector<std::optional<long long>> dimensions;
    dimensions.reserve(variable.dimensions.size());
    for (const auto& dimension : variable.dimensions) {
      if (!dimension) {
        dimensions.push_back(std::nullopt);
        continue;
      }
      dimensions.push_back(tryEvaluateSyntacticIntegerConstant(*dimension));
    }
    return dimensions;
  }

  std::vector<std::optional<long long>> buildResolvedArrayDimensions(
      const VariableDecl& variable) const {
    std::vector<std::optional<long long>> dimensions;
    dimensions.reserve(variable.dimensions.size());
    for (const auto& dimension : variable.dimensions) {
      if (!dimension) {
        dimensions.push_back(std::nullopt);
        continue;
      }
      dimensions.push_back(tryEvaluateIntegerConstant(*dimension));
    }
    return dimensions;
  }

  CheckedType syntacticVariableType(const VariableDecl& variable) const {
    return arrayType(variable.type.type.kind, buildSyntacticArrayDimensions(variable),
                     variable.type.isConst);
  }

  CheckedType resolvedVariableType(const VariableDecl& variable) const {
    return arrayType(variable.type.type.kind, buildResolvedArrayDimensions(variable),
                     variable.type.isConst);
  }

  void registerBuiltinFunction(const std::string& name, CheckedType returnType,
                               std::vector<CheckedType> parameters) {
    functions_.emplace(name, FunctionInfo{{}, returnType, std::move(parameters)});
    scopes_.front().emplace(name, SymbolInfo{SymbolKind::Function, {}, returnType, std::nullopt});
  }

  void registerBuiltins() {
    registerBuiltinFunction("getint", scalarType(TypeKind::Int), {});
    registerBuiltinFunction("getch", scalarType(TypeKind::Int), {});
    registerBuiltinFunction("getfloat", scalarType(TypeKind::Float), {});
    registerBuiltinFunction("getarray", scalarType(TypeKind::Int),
                            {arrayType(TypeKind::Int, {std::nullopt})});
    registerBuiltinFunction("getfarray", scalarType(TypeKind::Int),
                            {arrayType(TypeKind::Float, {std::nullopt})});
    registerBuiltinFunction("putint", scalarType(TypeKind::Void),
                            {scalarType(TypeKind::Int)});
    registerBuiltinFunction("putch", scalarType(TypeKind::Void),
                            {scalarType(TypeKind::Int)});
    registerBuiltinFunction("putfloat", scalarType(TypeKind::Void),
                            {scalarType(TypeKind::Float)});
    registerBuiltinFunction("putarray", scalarType(TypeKind::Void),
                            {scalarType(TypeKind::Int),
                             arrayType(TypeKind::Int, {std::nullopt})});
    registerBuiltinFunction("putfarray", scalarType(TypeKind::Void),
                            {scalarType(TypeKind::Int),
                             arrayType(TypeKind::Float, {std::nullopt})});
    registerBuiltinFunction("starttime", scalarType(TypeKind::Void), {});
    registerBuiltinFunction("stoptime", scalarType(TypeKind::Void), {});
  }

  void collectGlobalDeclarations() {
    for (const auto& declaration : unit_.declarations) {
      if (const auto* group = dynamic_cast<const VariableDeclGroup*>(declaration.get())) {
        for (const auto& variable : group->declarations) {
          declareInCurrentScope(variable->name, SymbolKind::Variable, variable->location(),
                                syntacticVariableType(*variable));
        }
        continue;
      }

      if (const auto* function = dynamic_cast<const FunctionDecl*>(declaration.get())) {
        if (functions_.find(function->name) == functions_.end()) {
          std::vector<CheckedType> parameters;
          parameters.reserve(function->parameters.size());
          for (const auto& parameter : function->parameters) {
            parameters.push_back(syntacticVariableType(*parameter));
          }
          functions_.emplace(function->name,
                             FunctionInfo{function->location(),
                                          scalarType(function->returnType.type.kind),
                                          std::move(parameters)});
        }
        scopes_.front().emplace(function->name,
                                SymbolInfo{SymbolKind::Function, function->location(),
                                           scalarType(function->returnType.type.kind),
                                           std::nullopt});
      }
    }
  }

  void analyzeGlobalDeclarations() {
    for (const auto& declaration : unit_.declarations) {
      if (const auto* group = dynamic_cast<const VariableDeclGroup*>(declaration.get())) {
        analyzeVariableGroup(*group, false);
        continue;
      }
      if (const auto* function = dynamic_cast<const FunctionDecl*>(declaration.get())) {
        analyzeFunction(*function);
      }
    }
  }

  void analyzeFunction(const FunctionDecl& function) {
    pushScope();
    currentFunctionName_ = function.name;
    currentReturnType_ = scalarType(function.returnType.type.kind);

    for (const auto& parameter : function.parameters) {
      if (!parameter->name.empty()) {
        declareInCurrentScope(parameter->name, SymbolKind::Variable, parameter->location(),
                              resolvedVariableType(*parameter));
      }
    }

    if (function.body) {
      analyzeStatement(*function.body);
    }

    currentFunctionName_.clear();
    currentReturnType_ = scalarType(TypeKind::Unknown);
    popScope();
  }

  void analyzeVariableGroup(const VariableDeclGroup& group, bool declareLocals) {
    for (const auto& variable : group.declarations) {
      analyzeVariable(*variable, declareLocals);
    }
  }

  void checkScalarInitializer(const VariableDecl& variable, const CheckedType& targetType,
                              const Expr& initializer) {
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&initializer)) {
      if (initList->elements.empty()) {
        diagnostics_.error(initializer.location(),
                           "scalar variable '" + variable.name +
                               "' cannot be initialized with an empty initializer list");
        return;
      }
      if (initList->elements.size() != 1) {
        diagnostics_.error(initializer.location(),
                           "scalar variable '" + variable.name +
                               "' cannot be initialized with multiple initializer elements");
        return;
      }
      checkScalarInitializer(variable, targetType, *initList->elements.front());
      return;
    }

    const CheckedType initializerType = analyzeExpression(initializer);
    if (initializerType.isUnknown()) {
      return;
    }
    if (initializerType.isArray()) {
      diagnostics_.error(initializer.location(),
                         "cannot initialize scalar variable '" + variable.name +
                             "' of type " + targetType.toString() +
                             " with array expression of type " + initializerType.toString());
      return;
    }
    if (!canImplicitlyConvert(initializerType, targetType)) {
      diagnostics_.error(initializer.location(),
                         "cannot initialize variable '" + variable.name + "' of type " +
                             targetType.toString() + " with expression of type " +
                             initializerType.toString());
    }
  }

  void checkArrayElementInitializer(const VariableDecl& variable, const CheckedType& elementType,
                                    const Expr& initializer) {
    const CheckedType initializerType = analyzeExpression(initializer);
    if (initializerType.isUnknown()) {
      return;
    }
    if (initializerType.isArray()) {
      diagnostics_.error(initializer.location(),
                         "cannot initialize element of array '" + variable.name +
                             "' with array expression of type " + initializerType.toString());
      return;
    }
    if (!canImplicitlyConvert(initializerType, elementType)) {
      diagnostics_.error(initializer.location(),
                         "cannot initialize element of array '" + variable.name + "' of type " +
                             elementType.toString() + " with expression of type " +
                             initializerType.toString());
    }
  }

  static std::optional<std::size_t> blockSizeForDepth(const CheckedType& type, std::size_t depth) {
    if (!type.isArray()) {
      return std::nullopt;
    }

    std::size_t blockSize = 1;
    for (std::size_t i = depth; i < type.arrayDimensions.size(); ++i) {
      if (!type.arrayDimensions[i].has_value() || *type.arrayDimensions[i] <= 0) {
        return std::nullopt;
      }
      blockSize *= static_cast<std::size_t>(*type.arrayDimensions[i]);
    }
    return blockSize;
  }

  void validateArrayInitializerLeaves(const VariableDecl& variable,
                                      const CheckedType& elementType, const Expr& initializer) {
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&initializer)) {
      for (const auto& element : initList->elements) {
        validateArrayInitializerLeaves(variable, elementType, *element);
      }
      return;
    }
    checkArrayElementInitializer(variable, elementType, initializer);
  }

  void validateArrayInitializerAligned(const VariableDecl& variable,
                                       const CheckedType& arrayType,
                                       const CheckedType& elementType, const Expr& initializer,
                                       std::size_t depth, std::size_t totalCapacity,
                                       std::size_t& linearPosition,
                                       bool& sawTotalOverflow) {
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&initializer)) {
      const auto blockSize = blockSizeForDepth(arrayType, depth);
      if (!blockSize.has_value()) {
        for (const auto& element : initList->elements) {
          validateArrayInitializerAligned(variable, arrayType, elementType, *element, depth + 1,
                                          totalCapacity, linearPosition, sawTotalOverflow);
        }
        return;
      }

      const std::size_t blockStart = (linearPosition / *blockSize) * *blockSize;
      const std::size_t blockEnd = std::min(totalCapacity, blockStart + *blockSize);
      if (initList->elements.empty()) {
        linearPosition = blockEnd;
        return;
      }

      for (const auto& element : initList->elements) {
        if (linearPosition >= totalCapacity) {
          if (!sawTotalOverflow) {
            diagnostics_.error(element->location(),
                               "too many initializer elements for array '" + variable.name + "'");
            sawTotalOverflow = true;
          }
          continue;
        }

        if (linearPosition >= blockEnd) {
          diagnostics_.error(element->location(),
                             "too many initializer elements for a subarray of '" +
                                 variable.name + "'");
          break;
        }

        validateArrayInitializerAligned(variable, arrayType, elementType, *element, depth + 1,
                                        totalCapacity, linearPosition, sawTotalOverflow);
      }

      if (linearPosition < blockEnd) {
        linearPosition = blockEnd;
      }
      return;
    }

    if (linearPosition >= totalCapacity) {
      if (!sawTotalOverflow) {
        diagnostics_.error(initializer.location(),
                           "too many initializer elements for array '" + variable.name + "'");
        sawTotalOverflow = true;
      }
      return;
    }

    checkArrayElementInitializer(variable, elementType, initializer);
    ++linearPosition;
  }

  void checkArrayInitializer(const VariableDecl& variable, const CheckedType& targetType,
                             const Expr& initializer) {
    const auto* initList = dynamic_cast<const InitListExpr*>(&initializer);
    if (initList == nullptr) {
      const CheckedType initializerType = analyzeExpression(initializer);
      diagnostics_.error(initializer.location(),
                         "array variable '" + variable.name +
                             "' must be initialized with an initializer list");
      if (!initializerType.isUnknown() && initializerType.isArray()) {
        diagnostics_.note(initializer.location(),
                          "found array expression of type " + initializerType.toString());
      }
      return;
    }

    const CheckedType elementType = elementTypeOf(targetType);
    if (const auto capacity = totalArrayCapacity(targetType); capacity.has_value()) {
      std::size_t linearPosition = 0;
      bool sawTotalOverflow = false;
      validateArrayInitializerAligned(variable, targetType, elementType, initializer, 0,
                                      static_cast<std::size_t>(*capacity), linearPosition,
                                      sawTotalOverflow);
      return;
    }

    validateArrayInitializerLeaves(variable, elementType, initializer);
  }

  void analyzeVariable(const VariableDecl& variable, bool declareLocals) {
    const CheckedType declaredType = resolvedVariableType(variable);
    std::optional<long long> integerConstantValue;

    if (variable.initializer) {
      if (declaredType.isArray()) {
        checkArrayInitializer(variable, declaredType, *variable.initializer);
      } else {
        checkScalarInitializer(variable, declaredType, *variable.initializer);
      }
    }

    if (variable.type.isConst && !declaredType.isArray() && isIntegralScalar(declaredType) &&
        variable.initializer) {
      integerConstantValue = tryEvaluateScalarIntegerInitializer(*variable.initializer);
    }

    if (declareLocals && !variable.name.empty()) {
      declareInCurrentScope(variable.name, SymbolKind::Variable, variable.location(), declaredType,
                            integerConstantValue);
      return;
    }

    if (auto* existing = lookupSymbolMutable(variable.name); existing != nullptr &&
        existing->kind == SymbolKind::Variable) {
      existing->type = declaredType;
      existing->integerConstantValue = integerConstantValue;
    }
  }

  void analyzeStatement(const Stmt& statement) {
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
      pushScope();
      for (const auto& nested : block->statements) {
        analyzeStatement(*nested);
      }
      popScope();
      return;
    }

    if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&statement)) {
      if (declStmt->declaration) {
        analyzeVariableGroup(*declStmt->declaration, true);
      }
      return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&statement)) {
      if (exprStmt->expression) {
        analyzeExpression(*exprStmt->expression);
      }
      return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
      analyzeReturnStatement(*returnStmt);
      return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
      checkConditionType(*ifStmt->condition, "if condition");
      if (ifStmt->thenBranch) {
        analyzeStatement(*ifStmt->thenBranch);
      }
      if (ifStmt->elseBranch) {
        analyzeStatement(*ifStmt->elseBranch);
      }
      return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
      pushScope();
      if (forStmt->init) {
        analyzeStatement(*forStmt->init);
      }
      if (forStmt->condition) {
        checkConditionType(*forStmt->condition, "for condition");
      }
      if (forStmt->update) {
        analyzeExpression(*forStmt->update);
      }
      if (forStmt->body) {
        analyzeStatement(*forStmt->body);
      }
      popScope();
      return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
      checkConditionType(*whileStmt->condition, "while condition");
      if (whileStmt->body) {
        analyzeStatement(*whileStmt->body);
      }
      return;
    }

    if (const auto* doWhileStmt = dynamic_cast<const DoWhileStmt*>(&statement)) {
      if (doWhileStmt->body) {
        analyzeStatement(*doWhileStmt->body);
      }
      checkConditionType(*doWhileStmt->condition, "do-while condition");
      return;
    }

    if (const auto* switchStmt = dynamic_cast<const SwitchStmt*>(&statement)) {
      const CheckedType conditionType = analyzeExpression(*switchStmt->condition);
      if (!conditionType.isUnknown() && !isIntegralScalar(conditionType)) {
        diagnostics_.error(switchStmt->condition->location(),
                           "switch condition must have an integral scalar type, got " +
                               conditionType.toString());
      }
      for (const auto& clause : switchStmt->clauses) {
        analyzeStatement(*clause);
      }
      return;
    }

    if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(&statement)) {
      analyzeExpression(*caseStmt->value);
      for (const auto& nested : caseStmt->statements) {
        analyzeStatement(*nested);
      }
      return;
    }

    if (const auto* defaultStmt = dynamic_cast<const DefaultStmt*>(&statement)) {
      for (const auto& nested : defaultStmt->statements) {
        analyzeStatement(*nested);
      }
    }
  }

  void analyzeReturnStatement(const ReturnStmt& statement) {
    if (currentReturnType_.kind == TypeKind::Void || currentReturnType_.isUnknown()) {
      return;
    }
    if (!statement.expression) {
      return;
    }

    const CheckedType expressionType = analyzeExpression(*statement.expression);
    if (!canImplicitlyConvert(expressionType, currentReturnType_)) {
      diagnostics_.error(statement.expression->location(),
                         "cannot return expression of type " + expressionType.toString() +
                             " from function '" + currentFunctionName_ +
                             "' returning " + currentReturnType_.toString());
    }
  }

  void checkConditionType(const Expr& expression, const char* context) {
    const CheckedType type = analyzeExpression(expression);
    if (!type.isUnknown() && !isNumericScalar(type)) {
      diagnostics_.error(expression.location(),
                         std::string(context) + " must have a scalar numeric type, got " +
                             type.toString());
    }
  }

  CheckedType analyzeExpression(const Expr& expression) {
    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* symbol = lookupSymbol(identifier->name);
      if (symbol == nullptr || symbol->kind != SymbolKind::Variable) {
        return scalarType(TypeKind::Unknown);
      }
      return symbol->type;
    }

    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      return scalarType(integer->isLongLong ? TypeKind::Long : TypeKind::Int);
    }

    if (dynamic_cast<const FloatLiteralExpr*>(&expression) != nullptr) {
      return scalarType(TypeKind::Float);
    }

    if (dynamic_cast<const StringLiteralExpr*>(&expression) != nullptr) {
      return scalarType(TypeKind::Unknown);
    }

    if (const auto* initList = dynamic_cast<const InitListExpr*>(&expression)) {
      for (const auto& element : initList->elements) {
        analyzeExpression(*element);
      }
      return scalarType(TypeKind::Unknown);
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      const CheckedType operandType = analyzeExpression(*unary->operand);
      if (operandType.isUnknown()) {
        return operandType;
      }

      switch (unary->op) {
        case TokenKind::LogicalNot:
          if (!isNumericScalar(operandType)) {
            diagnostics_.error(unary->location(),
                               "operator '!' requires a scalar numeric operand, got " +
                                   operandType.toString());
            return scalarType(TypeKind::Unknown);
          }
          return scalarType(TypeKind::Bool);
        case TokenKind::Plus:
        case TokenKind::Minus:
          if (!isNumericScalar(operandType)) {
            diagnostics_.error(unary->location(),
                               "operator '" + toString(unary->op) +
                                   "' requires a scalar numeric operand, got " +
                                   operandType.toString());
            return scalarType(TypeKind::Unknown);
          }
          return promoteArithmetic(operandType);
        case TokenKind::Ampersand:
        case TokenKind::Star:
          diagnostics_.error(unary->location(),
                             "pointer-style unary operator '" + toString(unary->op) +
                                 "' is not supported by the current type checker");
          return scalarType(TypeKind::Unknown);
        default:
          return scalarType(TypeKind::Unknown);
      }
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Comma) {
        analyzeExpression(*binary->lhs);
        return analyzeExpression(*binary->rhs);
      }

      if (binary->op == TokenKind::Assign) {
        const CheckedType lhsType = analyzeExpression(*binary->lhs);
        const CheckedType rhsType = analyzeExpression(*binary->rhs);
        if (lhsType.isUnknown() || rhsType.isUnknown()) {
          return lhsType;
        }
        if (lhsType.isConst) {
          diagnostics_.error(binary->lhs->location(),
                             "cannot assign to const expression of type " +
                                 lhsType.toString());
          return lhsType;
        }
        if (lhsType.isArray()) {
          diagnostics_.error(binary->lhs->location(),
                             "array value of type " + lhsType.toString() +
                                 " is not assignable");
          return lhsType;
        }
        if (!canImplicitlyConvert(rhsType, lhsType)) {
          diagnostics_.error(binary->rhs->location(),
                             "cannot assign expression of type " + rhsType.toString() +
                                 " to " + lhsType.toString());
        }
        return lhsType;
      }

      const CheckedType lhsType = analyzeExpression(*binary->lhs);
      const CheckedType rhsType = analyzeExpression(*binary->rhs);
      if (lhsType.isUnknown() || rhsType.isUnknown()) {
        return scalarType(TypeKind::Unknown);
      }

      switch (binary->op) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
          if (!isNumericScalar(lhsType) || !isNumericScalar(rhsType)) {
            diagnostics_.error(binary->location(),
                               "operator '" + toString(binary->op) +
                                   "' requires scalar numeric operands, got " +
                                   lhsType.toString() + " and " + rhsType.toString());
            return scalarType(TypeKind::Unknown);
          }
          return commonArithmeticType(promoteArithmetic(lhsType), promoteArithmetic(rhsType));
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe:
          if (!isIntegralScalar(lhsType) || !isIntegralScalar(rhsType)) {
            diagnostics_.error(binary->location(),
                               "operator '" + toString(binary->op) +
                                   "' requires scalar integral operands, got " +
                                   lhsType.toString() + " and " + rhsType.toString());
            return scalarType(TypeKind::Unknown);
          }
          return commonArithmeticType(promoteArithmetic(lhsType), promoteArithmetic(rhsType));
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::EqualEqual:
        case TokenKind::NotEqual:
          if (!isNumericScalar(lhsType) || !isNumericScalar(rhsType)) {
            diagnostics_.error(binary->location(),
                               "comparison operator '" + toString(binary->op) +
                                   "' requires scalar numeric operands, got " +
                                   lhsType.toString() + " and " + rhsType.toString());
            return scalarType(TypeKind::Unknown);
          }
          return scalarType(TypeKind::Bool);
        case TokenKind::LogicalAnd:
        case TokenKind::LogicalOr:
          if (!isNumericScalar(lhsType) || !isNumericScalar(rhsType)) {
            diagnostics_.error(binary->location(),
                               "logical operator '" + toString(binary->op) +
                                   "' requires scalar numeric operands, got " +
                                   lhsType.toString() + " and " + rhsType.toString());
            return scalarType(TypeKind::Unknown);
          }
          return scalarType(TypeKind::Bool);
        default:
          return scalarType(TypeKind::Unknown);
      }
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
      const FunctionInfo* function = nullptr;
      if (const auto* callee = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
        function = lookupFunction(callee->name);
      }

      std::vector<CheckedType> argumentTypes;
      argumentTypes.reserve(call->arguments.size());
      for (const auto& argument : call->arguments) {
        argumentTypes.push_back(analyzeExpression(*argument));
      }

      if (function != nullptr && function->parameters.size() == argumentTypes.size()) {
        for (std::size_t i = 0; i < argumentTypes.size(); ++i) {
          if (!canPassArgument(argumentTypes[i], function->parameters[i])) {
            diagnostics_.error(call->arguments[i]->location(),
                               "function argument " + std::to_string(i + 1) + " has type " +
                                   argumentTypes[i].toString() + ", expected " +
                                   function->parameters[i].toString());
          }
        }
        return function->returnType;
      }

      return function != nullptr ? function->returnType : scalarType(TypeKind::Unknown);
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
      const CheckedType baseType = analyzeExpression(*index->base);
      const CheckedType indexType = analyzeExpression(*index->index);
      if (!indexType.isUnknown() && !isIntegralScalar(indexType)) {
        diagnostics_.error(index->index->location(),
                           "array index must have an integral scalar type, got " +
                               indexType.toString());
      }
      if (baseType.isUnknown()) {
        return scalarType(TypeKind::Unknown);
      }
      if (!baseType.isArray()) {
        diagnostics_.error(index->base->location(),
                           "subscripted expression must be an array, got " +
                               baseType.toString());
        return scalarType(TypeKind::Unknown);
      }

      std::vector<std::optional<long long>> subDimensions;
      if (!baseType.arrayDimensions.empty()) {
        subDimensions.assign(baseType.arrayDimensions.begin() + 1, baseType.arrayDimensions.end());
      }
      return CheckedType{baseType.kind, baseType.arrayRank - 1, std::move(subDimensions),
                         baseType.isConst};
    }

    return scalarType(TypeKind::Unknown);
  }

  const TranslationUnit& unit_;
  DiagnosticEngine& diagnostics_;
  std::vector<std::unordered_map<std::string, SymbolInfo>> scopes_;
  std::unordered_map<std::string, FunctionInfo> functions_;
  std::string currentFunctionName_;
  CheckedType currentReturnType_ = scalarType(TypeKind::Unknown);
};

}  // namespace

std::string Type::toString() const {
  switch (kind) {
    case TypeKind::Void:
      return "void";
    case TypeKind::Int:
      return "int";
    case TypeKind::Float:
      return "float";
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Long:
      return "long";
    case TypeKind::Unknown:
      return "unknown";
  }
  return "unknown";
}

bool TypeChecker::check(const compiler::frontend::ast::TranslationUnit& unit,
                        compiler::support::DiagnosticEngine& diagnostics) const {
  return Checker(unit, diagnostics).run();
}

}  // namespace compiler::frontend::types
