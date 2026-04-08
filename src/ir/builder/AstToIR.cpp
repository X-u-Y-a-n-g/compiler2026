#include "compiler/ir/builder/AstToIR.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compiler/frontend/ast/AstNode.h"
#include "compiler/frontend/lexer/Token.h"
#include "compiler/ir/builder/IRBuilder.h"

namespace compiler::ir::builder {

namespace {

using compiler::frontend::ast::BinaryExpr;
using compiler::frontend::ast::BlockStmt;
using compiler::frontend::ast::BreakStmt;
using compiler::frontend::ast::CallExpr;
using compiler::frontend::ast::CaseStmt;
using compiler::frontend::ast::ContinueStmt;
using compiler::frontend::ast::Decl;
using compiler::frontend::ast::DeclStmt;
using compiler::frontend::ast::DefaultStmt;
using compiler::frontend::ast::DoWhileStmt;
using compiler::frontend::ast::Expr;
using compiler::frontend::ast::ExprStmt;
using compiler::frontend::ast::FloatLiteralExpr;
using compiler::frontend::ast::ForStmt;
using compiler::frontend::ast::FunctionDecl;
using compiler::frontend::ast::GotoStmt;
using compiler::frontend::ast::IdentifierExpr;
using compiler::frontend::ast::IfStmt;
using compiler::frontend::ast::IndexExpr;
using compiler::frontend::ast::InitListExpr;
using compiler::frontend::ast::IntegerLiteral;
using compiler::frontend::ast::LabelStmt;
using compiler::frontend::ast::NormalizedInitializerSlot;
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
using compiler::frontend::types::TypeKind;

struct LoweredType {
  TypeKind kind = TypeKind::Unknown;
  std::size_t arrayRank = 0;
  std::vector<std::optional<long long>> dimensions;
  bool isConst = false;

  [[nodiscard]] bool isArray() const { return arrayRank > 0; }
  [[nodiscard]] bool isVoid() const { return !isArray() && kind == TypeKind::Void; }
  [[nodiscard]] bool isBool() const { return !isArray() && kind == TypeKind::Bool; }
  [[nodiscard]] bool isFloat() const { return !isArray() && kind == TypeKind::Float; }
  [[nodiscard]] bool isIntegral() const {
    return !isArray() &&
           (kind == TypeKind::Int || kind == TypeKind::Long || kind == TypeKind::Bool);
  }
  [[nodiscard]] bool isNumeric() const { return isIntegral() || isFloat(); }

  [[nodiscard]] LoweredType elementType() const {
    if (!isArray()) {
      return *this;
    }
    LoweredType element = *this;
    element.arrayRank -= 1;
    if (!element.dimensions.empty()) {
      element.dimensions.erase(element.dimensions.begin());
    }
    return element;
  }

  [[nodiscard]] std::string toString() const {
    std::string text;
    switch (kind) {
      case TypeKind::Void:
        text = "void";
        break;
      case TypeKind::Int:
        text = "int";
        break;
      case TypeKind::Float:
        text = "float";
        break;
      case TypeKind::Bool:
        text = "bool";
        break;
      case TypeKind::Long:
        text = "long";
        break;
      case TypeKind::Unknown:
        text = "unknown";
        break;
    }
    for (const auto& dimension : dimensions) {
      if (dimension.has_value()) {
        text += "[" + std::to_string(*dimension) + "]";
      } else {
        text += "[]";
      }
    }
    return text;
  }
};

struct ConstantValue {
  LoweredType type;
  long long integer = 0;
  double floating = 0.0;

  [[nodiscard]] bool isFloat() const { return type.kind == TypeKind::Float; }
};

struct FunctionInfo {
  LoweredType returnType;
  std::vector<LoweredType> parameters;
};

struct VariableInfo {
  LoweredType type;
  std::string storage;
  bool isGlobal = false;
  bool isArrayParameter = false;
  std::optional<long long> integerConstantValue;
  std::optional<ConstantValue> constantValue;
};

struct LoweredValue {
  std::string repr;
  LoweredType type;
};

struct LoweredReference {
  std::string address;
  LoweredType type;
};

class Lowerer {
 public:
  explicit Lowerer(const TranslationUnit& unit) : unit_(unit) {}

  compiler::ir::core::Module run(const std::string& sourceName,
                                 const LoweringContext& context) {
    compiler::ir::core::Module module(sourceName);
    module_ = &module;

    pushScope();
    registerBuiltinFunctions();
    collectFunctionSignatures();
    lowerGlobals();
    lowerFunctions(context);
    popScope();

    module_ = nullptr;
    return module;
  }

 private:
  void pushScope() { scopes_.emplace_back(); }

  void popScope() {
    if (!scopes_.empty()) {
      scopes_.pop_back();
    }
  }

  bool declareVariable(const std::string& name, VariableInfo info) {
    if (name.empty()) {
      return false;
    }
    scopes_.back()[name] = std::move(info);
    return true;
  }

  const VariableInfo* lookupVariable(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  static bool isIntegerImmediate(const std::string& text) {
    if (text.empty()) {
      return false;
    }
    std::size_t index = 0;
    if (text[index] == '+' || text[index] == '-') {
      ++index;
    }
    if (index >= text.size()) {
      return false;
    }
    for (; index < text.size(); ++index) {
      if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
        return false;
      }
    }
    return true;
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

  static bool parseFloatLiteral(const std::string& text, double& value) {
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0') {
      return false;
    }
    value = parsed;
    return true;
  }

  static LoweredType promotedArithmeticType(const LoweredType& type) {
    if (type.isBool()) {
      return LoweredType{TypeKind::Int, 0, {}, false};
    }
    return LoweredType{type.kind, 0, {}, false};
  }

  static std::string zeroLiteral(const LoweredType& type) {
    return type.isFloat() ? "0.0" : "0";
  }

  static LoweredType flattenedElementType(LoweredType type) {
    while (type.isArray()) {
      type = type.elementType();
    }
    return type;
  }

  static bool typesMatch(const LoweredType& lhs, const LoweredType& rhs) {
    return lhs.kind == rhs.kind && lhs.arrayRank == rhs.arrayRank &&
           lhs.dimensions == rhs.dimensions;
  }

  static std::string tokenText(TokenKind kind) {
    switch (kind) {
      case TokenKind::Plus:
        return "+";
      case TokenKind::Minus:
        return "-";
      case TokenKind::Star:
        return "*";
      case TokenKind::Slash:
        return "/";
      case TokenKind::Percent:
        return "%";
      case TokenKind::Less:
        return "<";
      case TokenKind::LessEqual:
        return "<=";
      case TokenKind::Greater:
        return ">";
      case TokenKind::GreaterEqual:
        return ">=";
      case TokenKind::EqualEqual:
        return "==";
      case TokenKind::NotEqual:
        return "!=";
      case TokenKind::LogicalAnd:
        return "&&";
      case TokenKind::LogicalOr:
        return "||";
      case TokenKind::LogicalNot:
        return "!";
      case TokenKind::Ampersand:
        return "&";
      case TokenKind::Pipe:
        return "|";
      case TokenKind::Assign:
        return "=";
      case TokenKind::Comma:
        return ",";
      default:
        return compiler::frontend::lexer::toString(kind);
    }
  }

  static std::string formatFloatLiteral(double value) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    std::string text = os.str();
    if (text.find_first_of(".eEpP") == std::string::npos) {
      text += ".0";
    }
    return text;
  }

  static std::string renderConstantLiteral(const ConstantValue& value) {
    return value.isFloat() ? formatFloatLiteral(value.floating)
                           : std::to_string(value.integer);
  }

  static bool isTruthy(const ConstantValue& value) {
    return value.isFloat() ? value.floating != 0.0 : value.integer != 0;
  }

  ConstantValue castConstant(ConstantValue value, const LoweredType& targetType) const {
    if (targetType.kind == TypeKind::Unknown || targetType.isArray() || targetType.isVoid()) {
      return value;
    }

    value.type = targetType;
    if (targetType.isFloat()) {
      value.floating = value.isFloat() ? value.floating : static_cast<double>(value.integer);
      return value;
    }

    if (targetType.isBool()) {
      const long long integer = value.isFloat() ? (value.floating != 0.0 ? 1 : 0)
                                                : (value.integer != 0 ? 1 : 0);
      value.integer = integer;
      value.floating = static_cast<double>(integer);
      return value;
    }

    long long integer = 0;
    if (value.isFloat()) {
      integer = static_cast<long long>(value.floating);
    } else {
      integer = value.integer;
    }
    value.integer = integer;
    value.floating = static_cast<double>(integer);
    return value;
  }

  std::optional<ConstantValue> evaluateConstantValue(const Expr& expression) const {
    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      long long value = 0;
      if (!parseIntegerLiteral(integer->value, value)) {
        return std::nullopt;
      }
      return ConstantValue{
          LoweredType{integer->isLongLong ? TypeKind::Long : TypeKind::Int, 0, {}, false},
          value,
          static_cast<double>(value),
      };
    }

    if (const auto* floating = dynamic_cast<const FloatLiteralExpr*>(&expression)) {
      double value = 0.0;
      if (!parseFloatLiteral(floating->value, value)) {
        return std::nullopt;
      }
      return ConstantValue{LoweredType{TypeKind::Float, 0, {}, false}, 0, value};
    }

    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* variable = lookupVariable(identifier->name);
      if (variable == nullptr || !variable->constantValue.has_value()) {
        return std::nullopt;
      }
      return variable->constantValue;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      auto operand = evaluateConstantValue(*unary->operand);
      if (!operand.has_value()) {
        return std::nullopt;
      }

      switch (unary->op) {
        case TokenKind::Plus:
          return castConstant(*operand, promotedArithmeticType(operand->type));
        case TokenKind::Minus: {
          auto value = castConstant(*operand, promotedArithmeticType(operand->type));
          if (value.isFloat()) {
            value.floating = -value.floating;
          } else {
            value.integer = -value.integer;
            value.floating = static_cast<double>(value.integer);
          }
          return value;
        }
        case TokenKind::LogicalNot: {
          const long long result = isTruthy(*operand) ? 0 : 1;
          return ConstantValue{LoweredType{TypeKind::Bool, 0, {}, false}, result,
                               static_cast<double>(result)};
        }
        default:
          return std::nullopt;
      }
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Assign || binary->op == TokenKind::Comma) {
        return std::nullopt;
      }

      auto lhs = evaluateConstantValue(*binary->lhs);
      auto rhs = evaluateConstantValue(*binary->rhs);
      if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
      }

      switch (binary->op) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash: {
          const LoweredType resultType =
              commonArithmeticType(promotedArithmeticType(lhs->type), promotedArithmeticType(rhs->type));
          lhs = castConstant(*lhs, resultType);
          rhs = castConstant(*rhs, resultType);
          if (resultType.isFloat()) {
            double result = 0.0;
            if (binary->op == TokenKind::Plus) {
              result = lhs->floating + rhs->floating;
            } else if (binary->op == TokenKind::Minus) {
              result = lhs->floating - rhs->floating;
            } else if (binary->op == TokenKind::Star) {
              result = lhs->floating * rhs->floating;
            } else {
              result = lhs->floating / rhs->floating;
            }
            return ConstantValue{resultType, static_cast<long long>(result), result};
          }
          if (binary->op == TokenKind::Slash && rhs->integer == 0) {
            return std::nullopt;
          }
          long long result = 0;
          if (binary->op == TokenKind::Plus) {
            result = lhs->integer + rhs->integer;
          } else if (binary->op == TokenKind::Minus) {
            result = lhs->integer - rhs->integer;
          } else if (binary->op == TokenKind::Star) {
            result = lhs->integer * rhs->integer;
          } else {
            result = lhs->integer / rhs->integer;
          }
          return ConstantValue{resultType, result, static_cast<double>(result)};
        }
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe: {
          const LoweredType resultType =
              commonArithmeticType(promotedArithmeticType(lhs->type), promotedArithmeticType(rhs->type));
          lhs = castConstant(*lhs, resultType);
          rhs = castConstant(*rhs, resultType);
          if (binary->op == TokenKind::Percent && rhs->integer == 0) {
            return std::nullopt;
          }
          long long result = 0;
          if (binary->op == TokenKind::Percent) {
            result = lhs->integer % rhs->integer;
          } else if (binary->op == TokenKind::Ampersand) {
            result = lhs->integer & rhs->integer;
          } else {
            result = lhs->integer | rhs->integer;
          }
          return ConstantValue{resultType, result, static_cast<double>(result)};
        }
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::EqualEqual:
        case TokenKind::NotEqual: {
          const LoweredType comparisonType =
              commonArithmeticType(promotedArithmeticType(lhs->type), promotedArithmeticType(rhs->type));
          lhs = castConstant(*lhs, comparisonType);
          rhs = castConstant(*rhs, comparisonType);
          bool result = false;
          if (comparisonType.isFloat()) {
            if (binary->op == TokenKind::Less) {
              result = lhs->floating < rhs->floating;
            } else if (binary->op == TokenKind::LessEqual) {
              result = lhs->floating <= rhs->floating;
            } else if (binary->op == TokenKind::Greater) {
              result = lhs->floating > rhs->floating;
            } else if (binary->op == TokenKind::GreaterEqual) {
              result = lhs->floating >= rhs->floating;
            } else if (binary->op == TokenKind::EqualEqual) {
              result = lhs->floating == rhs->floating;
            } else {
              result = lhs->floating != rhs->floating;
            }
          } else {
            if (binary->op == TokenKind::Less) {
              result = lhs->integer < rhs->integer;
            } else if (binary->op == TokenKind::LessEqual) {
              result = lhs->integer <= rhs->integer;
            } else if (binary->op == TokenKind::Greater) {
              result = lhs->integer > rhs->integer;
            } else if (binary->op == TokenKind::GreaterEqual) {
              result = lhs->integer >= rhs->integer;
            } else if (binary->op == TokenKind::EqualEqual) {
              result = lhs->integer == rhs->integer;
            } else {
              result = lhs->integer != rhs->integer;
            }
          }
          const long long integerResult = result ? 1 : 0;
          return ConstantValue{LoweredType{TypeKind::Bool, 0, {}, false}, integerResult,
                               static_cast<double>(integerResult)};
        }
        case TokenKind::LogicalAnd:
        case TokenKind::LogicalOr: {
          const bool lhsTruthy = isTruthy(*lhs);
          const bool rhsTruthy = isTruthy(*rhs);
          const bool result =
              binary->op == TokenKind::LogicalAnd ? (lhsTruthy && rhsTruthy)
                                                  : (lhsTruthy || rhsTruthy);
          const long long integerResult = result ? 1 : 0;
          return ConstantValue{LoweredType{TypeKind::Bool, 0, {}, false}, integerResult,
                               static_cast<double>(integerResult)};
        }
        default:
          return std::nullopt;
      }
    }

    return std::nullopt;
  }

  std::optional<ConstantValue> constantFromNormalizedScalar(
      const VariableDecl& variable, const LoweredType& targetType) const {
    if (!variable.normalizedInitializer.available || variable.normalizedInitializer.slots.empty()) {
      return std::nullopt;
    }
    const auto& slot = variable.normalizedInitializer.slots.front();
    if (slot.isImplicitZero()) {
      ConstantValue zero{targetType, 0, 0.0};
      return castConstant(zero, targetType);
    }
    auto value = evaluateConstantValue(*slot.expression);
    if (!value.has_value()) {
      return std::nullopt;
    }
    return castConstant(*value, targetType);
  }

  std::string renderConstantOrExpression(const Expr& expression, const LoweredType& targetType) const {
    if (targetType.isIntegral()) {
      if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
        return integer->value;
      }
    }
    if (targetType.isFloat()) {
      if (const auto* floating = dynamic_cast<const FloatLiteralExpr*>(&expression)) {
        return floating->value;
      }
    }
    if (auto value = evaluateConstantValue(expression); value.has_value()) {
      return renderConstantLiteral(castConstant(*value, targetType));
    }
    return renderExpression(expression);
  }

  std::optional<long long> evaluateIntegerConstant(const Expr& expression) const {
    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      try {
        return std::stoll(integer->value, nullptr, 0);
      } catch (...) {
        return std::nullopt;
      }
    }

    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* variable = lookupVariable(identifier->name);
      if (variable == nullptr || !variable->integerConstantValue.has_value()) {
        return std::nullopt;
      }
      return variable->integerConstantValue;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      const auto operand = evaluateIntegerConstant(*unary->operand);
      if (!operand.has_value()) {
        return std::nullopt;
      }
      switch (unary->op) {
        case TokenKind::Plus:
          return operand;
        case TokenKind::Minus:
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
      const auto lhs = evaluateIntegerConstant(*binary->lhs);
      const auto rhs = evaluateIntegerConstant(*binary->rhs);
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
          return *rhs == 0 ? std::nullopt : std::optional<long long>(*lhs / *rhs);
        case TokenKind::Percent:
          return *rhs == 0 ? std::nullopt : std::optional<long long>(*lhs % *rhs);
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

  LoweredType resolveVariableType(const VariableDecl& variable) const {
    LoweredType type;
    type.kind = variable.type.type.kind;
    type.isConst = variable.type.isConst;
    type.arrayRank = variable.dimensions.size();
    type.dimensions.reserve(variable.dimensions.size());
    for (const auto& dimension : variable.dimensions) {
      if (!dimension) {
        type.dimensions.push_back(std::nullopt);
        continue;
      }
      type.dimensions.push_back(evaluateIntegerConstant(*dimension));
    }
    return type;
  }

  static std::string escapeGlobalName(const std::string& name) { return "@" + name; }

  std::string nextTemp(const char* hint = "t") {
    return "%" + std::string(hint) + std::to_string(tempCounter_++);
  }

  compiler::ir::core::BasicBlock& createBlock(const std::string& prefix) {
    return builder_->createBlock(prefix + "." + std::to_string(blockCounter_++));
  }

  bool isCurrentBlockTerminated() const {
    return currentBlock_ != nullptr && currentBlock_->isTerminated();
  }

  void ensureCurrentBlock(const std::string& prefix = "dead") {
    if (currentBlock_ == nullptr) {
      currentBlock_ = &createBlock(prefix);
    }
  }

  void emit(std::string opcode, std::vector<std::string> operands = {}) {
    ensureCurrentBlock();
    builder_->emit(*currentBlock_, std::move(opcode), std::move(operands));
  }

  void emitNamedValue(std::string result, std::string opcode,
                      std::vector<std::string> operands = {}) {
    ensureCurrentBlock();
    builder_->emit(*currentBlock_, std::move(opcode), std::move(operands), std::move(result));
  }

  std::string emitValue(std::string opcode, std::vector<std::string> operands = {},
                        const char* hint = "t") {
    const std::string temp = nextTemp(hint);
    emitNamedValue(temp, std::move(opcode), std::move(operands));
    return temp;
  }

  void emitJump(compiler::ir::core::BasicBlock& target) {
    if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
      emit("jmp", {target.name()});
    }
    currentBlock_ = nullptr;
  }

  void registerBuiltinFunctions() {
    auto scalar = [](TypeKind kind) {
      return LoweredType{kind, 0, {}, false};
    };
    auto array = [](TypeKind kind) {
      return LoweredType{kind, 1, {std::nullopt}, false};
    };

    functions_.emplace("getint", FunctionInfo{scalar(TypeKind::Int), {}});
    functions_.emplace("getch", FunctionInfo{scalar(TypeKind::Int), {}});
    functions_.emplace("getfloat", FunctionInfo{scalar(TypeKind::Float), {}});
    functions_.emplace("getarray", FunctionInfo{scalar(TypeKind::Int), {array(TypeKind::Int)}});
    functions_.emplace("getfarray", FunctionInfo{scalar(TypeKind::Int), {array(TypeKind::Float)}});
    functions_.emplace("putint", FunctionInfo{scalar(TypeKind::Void), {scalar(TypeKind::Int)}});
    functions_.emplace("putch", FunctionInfo{scalar(TypeKind::Void), {scalar(TypeKind::Int)}});
    functions_.emplace("putfloat", FunctionInfo{scalar(TypeKind::Void), {scalar(TypeKind::Float)}});
    functions_.emplace("putarray",
                       FunctionInfo{scalar(TypeKind::Void),
                                    {scalar(TypeKind::Int), array(TypeKind::Int)}});
    functions_.emplace("putfarray",
                       FunctionInfo{scalar(TypeKind::Void),
                                    {scalar(TypeKind::Int), array(TypeKind::Float)}});
    functions_.emplace("starttime", FunctionInfo{scalar(TypeKind::Void), {}});
    functions_.emplace("stoptime", FunctionInfo{scalar(TypeKind::Void), {}});
  }

  void collectFunctionSignatures() {
    for (const auto& declaration : unit_.declarations) {
      const auto* function = dynamic_cast<const FunctionDecl*>(declaration.get());
      if (function == nullptr) {
        continue;
      }
      FunctionInfo info;
      info.returnType.kind = function->returnType.type.kind;
      for (const auto& parameter : function->parameters) {
        info.parameters.push_back(resolveVariableType(*parameter));
      }
      functions_[function->name] = std::move(info);
    }
  }

  std::string renderExpression(const Expr& expression) const {
    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      return identifier->name;
    }
    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      return integer->value;
    }
    if (const auto* floating = dynamic_cast<const FloatLiteralExpr*>(&expression)) {
      return floating->value;
    }
    if (const auto* stringLiteral = dynamic_cast<const StringLiteralExpr*>(&expression)) {
      return '"' + stringLiteral->value + '"';
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      return "(" + tokenText(unary->op) + renderExpression(*unary->operand) + ")";
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      return "(" + renderExpression(*binary->lhs) + " " + tokenText(binary->op) + " " +
             renderExpression(*binary->rhs) + ")";
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
      std::ostringstream os;
      os << renderExpression(*call->callee) << "(";
      for (std::size_t i = 0; i < call->arguments.size(); ++i) {
        if (i != 0) {
          os << ", ";
        }
        os << renderExpression(*call->arguments[i]);
      }
      os << ")";
      return os.str();
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
      return renderExpression(*index->base) + "[" + renderExpression(*index->index) + "]";
    }
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&expression)) {
      std::ostringstream os;
      os << "{";
      for (std::size_t i = 0; i < initList->elements.size(); ++i) {
        if (i != 0) {
          os << ", ";
        }
        os << renderExpression(*initList->elements[i]);
      }
      os << "}";
      return os.str();
    }
    return "<expr>";
  }

  compiler::ir::core::Constant buildZeroConstant(const LoweredType& type) const {
    if (!type.isArray()) {
      return compiler::ir::core::Constant::scalar(type.toString(), zeroLiteral(type));
    }

    std::vector<compiler::ir::core::Constant> elements;
    if (!type.dimensions.empty() && type.dimensions.front().has_value()) {
      elements.reserve(static_cast<std::size_t>(*type.dimensions.front()));
      for (long long i = 0; i < *type.dimensions.front(); ++i) {
        elements.push_back(buildZeroConstant(type.elementType()));
      }
    }
    return compiler::ir::core::Constant::aggregate(type.toString(), std::move(elements));
  }

  compiler::ir::core::Constant buildScalarConstant(const Expr* expression,
                                                   const LoweredType& type) const {
    if (expression == nullptr) {
      return compiler::ir::core::Constant::scalar(type.toString(), zeroLiteral(type));
    }
    return compiler::ir::core::Constant::scalar(type.toString(),
                                                renderConstantOrExpression(*expression, type));
  }

  compiler::ir::core::Constant buildAggregateConstant(
      const std::vector<NormalizedInitializerSlot>& slots, const LoweredType& type,
      std::size_t& slotIndex) const {
    if (!type.isArray()) {
      const Expr* expression = slotIndex < slots.size() ? slots[slotIndex].expression : nullptr;
      if (slotIndex < slots.size()) {
        ++slotIndex;
      }
      return buildScalarConstant(expression, type);
    }

    std::vector<compiler::ir::core::Constant> elements;
    if (!type.dimensions.empty() && type.dimensions.front().has_value()) {
      elements.reserve(static_cast<std::size_t>(*type.dimensions.front()));
      for (long long i = 0; i < *type.dimensions.front(); ++i) {
        elements.push_back(buildAggregateConstant(slots, type.elementType(), slotIndex));
      }
    }
    return compiler::ir::core::Constant::aggregate(type.toString(), std::move(elements));
  }

  compiler::ir::core::Constant buildGlobalInitializer(const VariableDecl& variable,
                                                      const LoweredType& type) const {
    if (!variable.normalizedInitializer.available || variable.normalizedInitializer.slots.empty()) {
      return buildZeroConstant(type);
    }

    if (!type.isArray()) {
      return buildScalarConstant(variable.normalizedInitializer.slots.front().expression, type);
    }

    std::size_t slotIndex = 0;
    return buildAggregateConstant(variable.normalizedInitializer.slots, type, slotIndex);
  }

  void lowerGlobals() {
    for (const auto& declaration : unit_.declarations) {
      const auto* group = dynamic_cast<const VariableDeclGroup*>(declaration.get());
      if (group == nullptr) {
        continue;
      }
      for (const auto& variable : group->declarations) {
        const LoweredType type = resolveVariableType(*variable);
        VariableInfo info;
        info.type = type;
        info.storage = escapeGlobalName(variable->name);
        info.isGlobal = true;
        if (variable->type.isConst && !type.isArray() && type.isIntegral() &&
            variable->initializer != nullptr) {
          info.integerConstantValue = evaluateIntegerConstant(*variable->initializer);
        }
        if (variable->type.isConst && !type.isArray()) {
          info.constantValue = constantFromNormalizedScalar(*variable, type);
        }
        declareVariable(variable->name, info);

        module_->addGlobal(compiler::ir::core::GlobalVariable(
            variable->name, type.toString(), buildGlobalInitializer(*variable, type),
            variable->type.isConst));
      }
    }
  }

  void lowerFunctions(const LoweringContext& context) {
    for (const auto& declaration : unit_.declarations) {
      const auto* function = dynamic_cast<const FunctionDecl*>(declaration.get());
      if (function == nullptr || function->body == nullptr) {
        continue;
      }
      lowerFunction(*function, context);
    }
  }

  void lowerFunction(const FunctionDecl& functionDecl, const LoweringContext&) {
    std::vector<compiler::ir::core::FunctionParameter> parameters;
    parameters.reserve(functionDecl.parameters.size());
    for (std::size_t i = 0; i < functionDecl.parameters.size(); ++i) {
      const auto& parameter = functionDecl.parameters[i];
      const std::string name =
          parameter->name.empty() ? "%arg" + std::to_string(i) : parameter->name;
      parameters.push_back({name, resolveVariableType(*parameter).toString()});
    }

    auto& function = module_->addFunction(compiler::ir::core::Function(
        functionDecl.name, functionDecl.returnType.type.toString(), std::move(parameters)));
    compiler::ir::builder::IRBuilder builder(function);
    builder_ = &builder;
    currentFunctionName_ = functionDecl.name;
    currentReturnType_.kind = functionDecl.returnType.type.kind;
    currentBlock_ = &builder_->createBlock("entry");
    tempCounter_ = 0;
    blockCounter_ = 0;
    labelBlocks_.clear();
    breakTargets_.clear();
    continueTargets_.clear();

    pushScope();

    for (std::size_t i = 0; i < functionDecl.parameters.size(); ++i) {
      const auto& parameter = functionDecl.parameters[i];
      const LoweredType parameterType = resolveVariableType(*parameter);
      const std::string argValue = "%arg" + std::to_string(i);
      emitNamedValue(argValue, "param", {parameterType.toString(), parameter->name});

      VariableInfo info;
      info.type = parameterType;
      if (parameterType.isArray()) {
        info.storage = argValue;
        info.isArrayParameter = true;
      } else {
        info.storage = emitValue("alloca", {parameterType.toString()}, "addr");
        emit("store", {parameterType.toString(), argValue, info.storage});
      }
      declareVariable(parameter->name, info);
    }

    lowerStatement(*functionDecl.body);
    if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
      if (currentReturnType_.isVoid()) {
        emit("ret");
      } else {
        emit("ret", {zeroLiteral(currentReturnType_)});
      }
      currentBlock_ = nullptr;
    }

    popScope();
    builder_ = nullptr;
    currentBlock_ = nullptr;
    currentFunctionName_.clear();
    currentReturnType_ = {};
  }

  LoweredType commonArithmeticType(const LoweredType& lhs, const LoweredType& rhs) const {
    if (lhs.kind == TypeKind::Float || rhs.kind == TypeKind::Float) {
      return LoweredType{TypeKind::Float, 0, {}, false};
    }
    if (lhs.kind == TypeKind::Long || rhs.kind == TypeKind::Long) {
      return LoweredType{TypeKind::Long, 0, {}, false};
    }
    return LoweredType{TypeKind::Int, 0, {}, false};
  }

  LoweredValue castValue(LoweredValue value, const LoweredType& targetType) {
    if (typesMatch(value.type, targetType) || targetType.kind == TypeKind::Unknown ||
        value.type.kind == TypeKind::Unknown || value.type.isArray() || targetType.isArray()) {
      value.type = targetType.kind == TypeKind::Unknown ? value.type : targetType;
      return value;
    }

    if (targetType.kind == TypeKind::Bool) {
      if (value.type.kind == TypeKind::Bool) {
        value.type = targetType;
        return value;
      }
      const std::string zero = value.type.isFloat() ? "0.0" : "0";
      const std::string opcode = value.type.isFloat() ? "fcmp.one" : "icmp.ne";
      return {emitValue(opcode, {value.repr, zero}, "bool"), targetType};
    }

    if (value.type.kind == TypeKind::Bool && targetType.kind != TypeKind::Bool) {
      const std::string opcode = targetType.kind == TypeKind::Float ? "uitofp" : "zext";
      return {emitValue(opcode, {value.repr, targetType.toString()}, "cast"), targetType};
    }

    if (value.type.isFloat() && !targetType.isFloat()) {
      return {emitValue("fptosi", {value.repr, targetType.toString()}, "cast"), targetType};
    }
    if (!value.type.isFloat() && targetType.isFloat()) {
      return {emitValue("sitofp", {value.repr, targetType.toString()}, "cast"), targetType};
    }
    if (value.type.kind != targetType.kind) {
      return {emitValue("cast", {value.repr, targetType.toString()}, "cast"), targetType};
    }

    value.type = targetType;
    return value;
  }

  LoweredReference lowerLValue(const Expr& expression) {
    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* variable = lookupVariable(identifier->name);
      if (variable != nullptr) {
        return {variable->storage, variable->type};
      }
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
      LoweredReference base = lowerLValue(*index->base);
      LoweredValue indexValue = castValue(lowerValue(*index->index),
                                          LoweredType{TypeKind::Int, 0, {}, false});
      std::string address = emitValue("gep", {base.address, indexValue.repr}, "gep");
      return {address, base.type.elementType()};
    }

    return {"<invalid-lvalue>", LoweredType{TypeKind::Unknown, 0, {}, false}};
  }

  LoweredValue materializeBooleanValue(const Expr& expression) {
    const LoweredType boolType{TypeKind::Bool, 0, {}, false};
    const std::string slot = emitValue("alloca", {boolType.toString()}, "bool.addr");
    auto& trueBlock = createBlock("logic.true");
    auto& falseBlock = createBlock("logic.false");
    auto& mergeBlock = createBlock("logic.merge");

    lowerConditionBranch(expression, trueBlock, falseBlock);

    currentBlock_ = &trueBlock;
    emit("store", {boolType.toString(), "1", slot});
    emitJump(mergeBlock);

    currentBlock_ = &falseBlock;
    emit("store", {boolType.toString(), "0", slot});
    emitJump(mergeBlock);

    currentBlock_ = &mergeBlock;
    return {emitValue("load", {boolType.toString(), slot}, "bool"), boolType};
  }

  void lowerConditionBranch(const Expr& expression, compiler::ir::core::BasicBlock& trueBlock,
                            compiler::ir::core::BasicBlock& falseBlock) {
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::LogicalAnd) {
        auto& rhsBlock = createBlock("land.rhs");
        lowerConditionBranch(*binary->lhs, rhsBlock, falseBlock);
        currentBlock_ = &rhsBlock;
        lowerConditionBranch(*binary->rhs, trueBlock, falseBlock);
        return;
      }
      if (binary->op == TokenKind::LogicalOr) {
        auto& rhsBlock = createBlock("lor.rhs");
        lowerConditionBranch(*binary->lhs, trueBlock, rhsBlock);
        currentBlock_ = &rhsBlock;
        lowerConditionBranch(*binary->rhs, trueBlock, falseBlock);
        return;
      }
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression);
        unary != nullptr && unary->op == TokenKind::LogicalNot) {
      lowerConditionBranch(*unary->operand, falseBlock, trueBlock);
      return;
    }

    LoweredValue condition = castValue(lowerValue(expression),
                                       LoweredType{TypeKind::Bool, 0, {}, false});
    emit("br", {condition.repr, trueBlock.name(), falseBlock.name()});
    currentBlock_ = nullptr;
  }

  LoweredValue lowerValue(const Expr& expression) {
    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* variable = lookupVariable(identifier->name);
      if (variable == nullptr) {
        return {"0", LoweredType{TypeKind::Unknown, 0, {}, false}};
      }
      if (!variable->type.isArray() && variable->constantValue.has_value()) {
        return {renderConstantLiteral(*variable->constantValue), variable->type};
      }
      if (variable->type.isArray()) {
        return {variable->storage, variable->type};
      }
      return {emitValue("load", {variable->type.toString(), variable->storage}, "load"),
              variable->type};
    }

    if (const auto* integer = dynamic_cast<const IntegerLiteral*>(&expression)) {
      return {integer->value,
              LoweredType{integer->isLongLong ? TypeKind::Long : TypeKind::Int, 0, {}, false}};
    }

    if (const auto* floating = dynamic_cast<const FloatLiteralExpr*>(&expression)) {
      return {floating->value, LoweredType{TypeKind::Float, 0, {}, false}};
    }

    if (dynamic_cast<const StringLiteralExpr*>(&expression) != nullptr) {
      return {renderExpression(expression), LoweredType{TypeKind::Unknown, 0, {}, false}};
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      if (unary->op == TokenKind::LogicalNot) {
        return materializeBooleanValue(expression);
      }

      LoweredValue operand = lowerValue(*unary->operand);
      if (unary->op == TokenKind::Plus) {
        operand = castValue(std::move(operand), promotedArithmeticType(operand.type));
        return operand;
      }
      if (unary->op == TokenKind::Minus) {
        operand = castValue(std::move(operand), promotedArithmeticType(operand.type));
        const std::string opcode = operand.type.isFloat() ? "fneg" : "neg";
        return {emitValue(opcode, {operand.repr}, "neg"), operand.type};
      }
      return operand;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Comma) {
        lowerValue(*binary->lhs);
        return lowerValue(*binary->rhs);
      }

      if (binary->op == TokenKind::Assign) {
        LoweredReference destination = lowerLValue(*binary->lhs);
        LoweredValue source = castValue(lowerValue(*binary->rhs), destination.type);
        emit("store", {destination.type.toString(), source.repr, destination.address});
        return source;
      }

      if (binary->op == TokenKind::LogicalAnd || binary->op == TokenKind::LogicalOr) {
        return materializeBooleanValue(expression);
      }

      LoweredValue lhs = lowerValue(*binary->lhs);
      LoweredValue rhs = lowerValue(*binary->rhs);

      switch (binary->op) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash: {
          const LoweredType resultType = commonArithmeticType(lhs.type, rhs.type);
          lhs = castValue(std::move(lhs), resultType);
          rhs = castValue(std::move(rhs), resultType);
          std::string opcode;
          if (binary->op == TokenKind::Plus) {
            opcode = resultType.isFloat() ? "fadd" : "add";
          } else if (binary->op == TokenKind::Minus) {
            opcode = resultType.isFloat() ? "fsub" : "sub";
          } else if (binary->op == TokenKind::Star) {
            opcode = resultType.isFloat() ? "fmul" : "mul";
          } else {
            opcode = resultType.isFloat() ? "fdiv" : "div";
          }
          return {emitValue(opcode, {lhs.repr, rhs.repr}), resultType};
        }
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe: {
          const LoweredType resultType = commonArithmeticType(lhs.type, rhs.type);
          lhs = castValue(std::move(lhs), resultType);
          rhs = castValue(std::move(rhs), resultType);
          std::string opcode = binary->op == TokenKind::Percent
                                   ? "mod"
                                   : (binary->op == TokenKind::Ampersand ? "and" : "or");
          return {emitValue(opcode, {lhs.repr, rhs.repr}), resultType};
        }
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::EqualEqual:
        case TokenKind::NotEqual: {
          const LoweredType comparisonType = commonArithmeticType(lhs.type, rhs.type);
          lhs = castValue(std::move(lhs), comparisonType);
          rhs = castValue(std::move(rhs), comparisonType);
          std::string opcode;
          const bool isFloat = comparisonType.isFloat();
          switch (binary->op) {
            case TokenKind::Less:
              opcode = isFloat ? "fcmp.olt" : "icmp.slt";
              break;
            case TokenKind::LessEqual:
              opcode = isFloat ? "fcmp.ole" : "icmp.sle";
              break;
            case TokenKind::Greater:
              opcode = isFloat ? "fcmp.ogt" : "icmp.sgt";
              break;
            case TokenKind::GreaterEqual:
              opcode = isFloat ? "fcmp.oge" : "icmp.sge";
              break;
            case TokenKind::EqualEqual:
              opcode = isFloat ? "fcmp.oeq" : "icmp.eq";
              break;
            case TokenKind::NotEqual:
              opcode = isFloat ? "fcmp.one" : "icmp.ne";
              break;
            default:
              opcode = "icmp.ne";
              break;
          }
          return {emitValue(opcode, {lhs.repr, rhs.repr}, "cmp"),
                  LoweredType{TypeKind::Bool, 0, {}, false}};
        }
        default:
          break;
      }
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
      std::string calleeName = renderExpression(*call->callee);
      if (const auto* calleeIdentifier = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
        calleeName = calleeIdentifier->name;
      }

      FunctionInfo signature;
      if (const auto found = functions_.find(calleeName); found != functions_.end()) {
        signature = found->second;
      }

      std::vector<std::string> operands;
      operands.push_back("@" + calleeName);
      for (std::size_t i = 0; i < call->arguments.size(); ++i) {
        LoweredValue argument = lowerValue(*call->arguments[i]);
        if (i < signature.parameters.size() && !signature.parameters[i].isArray()) {
          argument = castValue(std::move(argument), signature.parameters[i]);
        }
        operands.push_back(argument.repr);
      }

      if (signature.returnType.isVoid()) {
        emit("call", operands);
        return {"", signature.returnType};
      }
      return {emitValue("call", operands, "call"), signature.returnType};
    }

    if (dynamic_cast<const InitListExpr*>(&expression) != nullptr) {
      return {"0", LoweredType{TypeKind::Unknown, 0, {}, false}};
    }

    if (dynamic_cast<const IndexExpr*>(&expression) != nullptr) {
      LoweredReference reference = lowerLValue(expression);
      if (reference.type.isArray()) {
        return {reference.address, reference.type};
      }
      return {emitValue("load", {reference.type.toString(), reference.address}, "load"),
              reference.type};
    }

    return {"0", LoweredType{TypeKind::Unknown, 0, {}, false}};
  }

  void emitLocalInitializer(const VariableDecl& variable, const VariableInfo& info) {
    if (!variable.normalizedInitializer.available || variable.normalizedInitializer.slots.empty()) {
      return;
    }

    auto lowerInitializerValue = [&](const Expr& expression, const LoweredType& targetType) {
      if (auto constant = evaluateConstantValue(expression); constant.has_value()) {
        return renderConstantLiteral(castConstant(*constant, targetType));
      }
      return castValue(lowerValue(expression), targetType).repr;
    };

    if (!info.type.isArray()) {
      const auto& slot = variable.normalizedInitializer.slots.front();
      const std::string value = slot.isImplicitZero()
                                    ? zeroLiteral(info.type)
                                    : lowerInitializerValue(*slot.expression, info.type);
      emit("store", {info.type.toString(), value, info.storage});
      return;
    }

    const LoweredType elementType = flattenedElementType(info.type);
    for (std::size_t i = 0; i < variable.normalizedInitializer.slots.size(); ++i) {
      const auto& slot = variable.normalizedInitializer.slots[i];
      std::string value = zeroLiteral(elementType);
      if (!slot.isImplicitZero()) {
        value = lowerInitializerValue(*slot.expression, elementType);
      }
      const std::string elementAddress =
          emitValue("gep.linear", {info.storage, std::to_string(i)}, "gep");
      emit("store", {elementType.toString(), value, elementAddress});
    }
  }

  void lowerLocalVariableGroup(const VariableDeclGroup& group) {
    for (const auto& variable : group.declarations) {
      const LoweredType type = resolveVariableType(*variable);
      VariableInfo info;
      info.type = type;
      info.storage = emitValue("alloca", {type.toString()}, "addr");
      if (variable->type.isConst && !type.isArray() && type.isIntegral() &&
          variable->initializer != nullptr) {
        info.integerConstantValue = evaluateIntegerConstant(*variable->initializer);
      }
      if (variable->type.isConst && !type.isArray()) {
        info.constantValue = constantFromNormalizedScalar(*variable, type);
      }
      declareVariable(variable->name, info);
      emitLocalInitializer(*variable, info);
    }
  }

  void lowerStatement(const Stmt& statement) {
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
      pushScope();
      for (const auto& nested : block->statements) {
        lowerStatement(*nested);
      }
      popScope();
      return;
    }

    if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&statement)) {
      if (declStmt->declaration) {
        lowerLocalVariableGroup(*declStmt->declaration);
      }
      return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&statement)) {
      if (exprStmt->expression) {
        lowerValue(*exprStmt->expression);
      }
      return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
      if (returnStmt->expression) {
        LoweredValue result = castValue(lowerValue(*returnStmt->expression), currentReturnType_);
        emit("ret", {result.repr});
      } else {
        emit("ret");
      }
      currentBlock_ = nullptr;
      return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
      auto& thenBlock = createBlock("if.then");
      auto& mergeBlock = createBlock("if.end");
      compiler::ir::core::BasicBlock* elseBlock = nullptr;
      if (ifStmt->elseBranch) {
        elseBlock = &createBlock("if.else");
      }
      lowerConditionBranch(*ifStmt->condition, thenBlock,
                           elseBlock != nullptr ? *elseBlock : mergeBlock);

      currentBlock_ = &thenBlock;
      if (ifStmt->thenBranch) {
        lowerStatement(*ifStmt->thenBranch);
      }
      if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
        emitJump(mergeBlock);
      }

      if (ifStmt->elseBranch) {
        currentBlock_ = elseBlock;
        lowerStatement(*ifStmt->elseBranch);
        if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
          emitJump(mergeBlock);
        }
      }

      currentBlock_ = &mergeBlock;
      return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
      auto& condBlock = createBlock("while.cond");
      auto& bodyBlock = createBlock("while.body");
      auto& endBlock = createBlock("while.end");
      emitJump(condBlock);
      currentBlock_ = &condBlock;
      lowerConditionBranch(*whileStmt->condition, bodyBlock, endBlock);

      breakTargets_.push_back(&endBlock);
      continueTargets_.push_back(&condBlock);
      currentBlock_ = &bodyBlock;
      if (whileStmt->body) {
        lowerStatement(*whileStmt->body);
      }
      if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
        emitJump(condBlock);
      }
      breakTargets_.pop_back();
      continueTargets_.pop_back();
      currentBlock_ = &endBlock;
      return;
    }

    if (const auto* doWhileStmt = dynamic_cast<const DoWhileStmt*>(&statement)) {
      auto& bodyBlock = createBlock("do.body");
      auto& condBlock = createBlock("do.cond");
      auto& endBlock = createBlock("do.end");
      emitJump(bodyBlock);

      breakTargets_.push_back(&endBlock);
      continueTargets_.push_back(&condBlock);
      currentBlock_ = &bodyBlock;
      if (doWhileStmt->body) {
        lowerStatement(*doWhileStmt->body);
      }
      if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
        emitJump(condBlock);
      }
      currentBlock_ = &condBlock;
      lowerConditionBranch(*doWhileStmt->condition, bodyBlock, endBlock);
      breakTargets_.pop_back();
      continueTargets_.pop_back();
      currentBlock_ = &endBlock;
      return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
      pushScope();
      if (forStmt->init) {
        lowerStatement(*forStmt->init);
      }
      auto& condBlock = createBlock("for.cond");
      auto& bodyBlock = createBlock("for.body");
      auto& updateBlock = createBlock("for.step");
      auto& endBlock = createBlock("for.end");
      emitJump(condBlock);

      currentBlock_ = &condBlock;
      if (forStmt->condition) {
        lowerConditionBranch(*forStmt->condition, bodyBlock, endBlock);
      } else {
        emit("jmp", {bodyBlock.name()});
        currentBlock_ = nullptr;
      }

      breakTargets_.push_back(&endBlock);
      continueTargets_.push_back(&updateBlock);
      currentBlock_ = &bodyBlock;
      if (forStmt->body) {
        lowerStatement(*forStmt->body);
      }
      if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
        emitJump(updateBlock);
      }

      currentBlock_ = &updateBlock;
      if (forStmt->update) {
        lowerValue(*forStmt->update);
      }
      if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
        emitJump(condBlock);
      }
      breakTargets_.pop_back();
      continueTargets_.pop_back();
      currentBlock_ = &endBlock;
      popScope();
      return;
    }

    if (dynamic_cast<const BreakStmt*>(&statement) != nullptr) {
      if (!breakTargets_.empty()) {
        emitJump(*breakTargets_.back());
      }
      return;
    }

    if (dynamic_cast<const ContinueStmt*>(&statement) != nullptr) {
      if (!continueTargets_.empty()) {
        emitJump(*continueTargets_.back());
      }
      return;
    }

    if (const auto* gotoStmt = dynamic_cast<const GotoStmt*>(&statement)) {
      auto& target = getOrCreateLabelBlock(gotoStmt->label);
      emitJump(target);
      return;
    }

    if (const auto* labelStmt = dynamic_cast<const LabelStmt*>(&statement)) {
      auto& labelBlock = getOrCreateLabelBlock(labelStmt->label);
      if (currentBlock_ != nullptr && currentBlock_ != &labelBlock && !isCurrentBlockTerminated()) {
        emitJump(labelBlock);
      }
      currentBlock_ = &labelBlock;
      if (labelStmt->statement) {
        lowerStatement(*labelStmt->statement);
      }
      return;
    }

    if (const auto* switchStmt = dynamic_cast<const SwitchStmt*>(&statement)) {
      lowerSwitch(*switchStmt);
      return;
    }

    if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(&statement)) {
      for (const auto& nested : caseStmt->statements) {
        lowerStatement(*nested);
      }
      return;
    }

    if (const auto* defaultStmt = dynamic_cast<const DefaultStmt*>(&statement)) {
      for (const auto& nested : defaultStmt->statements) {
        lowerStatement(*nested);
      }
    }
  }

  compiler::ir::core::BasicBlock& getOrCreateLabelBlock(const std::string& label) {
    if (const auto found = labelBlocks_.find(label); found != labelBlocks_.end()) {
      return *found->second;
    }
    auto& block = builder_->createBlock("label." + label);
    labelBlocks_.emplace(label, &block);
    return block;
  }

  void lowerSwitch(const SwitchStmt& switchStmt) {
    struct ClauseInfo {
      const Stmt* statement = nullptr;
      compiler::ir::core::BasicBlock* block = nullptr;
    };

    LoweredValue condition = lowerValue(*switchStmt.condition);
    std::vector<ClauseInfo> clauses;
    clauses.reserve(switchStmt.clauses.size());
    compiler::ir::core::BasicBlock* defaultBlock = nullptr;
    auto& endBlock = createBlock("switch.end");

    for (const auto& clause : switchStmt.clauses) {
      auto& block = createBlock("switch.case");
      if (dynamic_cast<const DefaultStmt*>(clause.get()) != nullptr) {
        defaultBlock = &block;
      }
      clauses.push_back({clause.get(), &block});
    }

    std::vector<const CaseStmt*> caseClauses;
    for (const auto& clause : clauses) {
      if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(clause.statement)) {
        caseClauses.push_back(caseStmt);
      }
    }

    if (caseClauses.empty()) {
      emitJump(defaultBlock != nullptr ? *defaultBlock : endBlock);
    } else {
      compiler::ir::core::BasicBlock* dispatchBlock = currentBlock_;
      std::size_t caseIndex = 0;
      for (std::size_t i = 0; i < clauses.size(); ++i) {
        const auto* caseStmt = dynamic_cast<const CaseStmt*>(clauses[i].statement);
        if (caseStmt == nullptr) {
          continue;
        }

        compiler::ir::core::BasicBlock* missBlock = nullptr;
        std::size_t remainingCases = caseClauses.size() - caseIndex - 1;
        if (remainingCases > 0) {
          missBlock = &createBlock("switch.dispatch");
        } else {
          missBlock = defaultBlock != nullptr ? defaultBlock : &endBlock;
        }

        currentBlock_ = dispatchBlock;
        const std::string caseValue = evaluateIntegerConstant(*caseStmt->value).has_value()
                                          ? std::to_string(*evaluateIntegerConstant(*caseStmt->value))
                                          : renderExpression(*caseStmt->value);
        const LoweredValue rhs = castValue({caseValue, LoweredType{TypeKind::Int, 0, {}, false}},
                                           condition.type);
        const std::string cmpOpcode = condition.type.isFloat() ? "fcmp.oeq" : "icmp.eq";
        const std::string cmp = emitValue(cmpOpcode, {condition.repr, rhs.repr}, "switch.cmp");
        emit("br", {cmp, clauses[i].block->name(), missBlock->name()});
        currentBlock_ = nullptr;
        dispatchBlock = missBlock;
        ++caseIndex;
      }
    }

    breakTargets_.push_back(&endBlock);
    for (std::size_t i = 0; i < clauses.size(); ++i) {
      currentBlock_ = clauses[i].block;
      if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(clauses[i].statement)) {
        for (const auto& nested : caseStmt->statements) {
          lowerStatement(*nested);
        }
      } else if (const auto* defaultStmt = dynamic_cast<const DefaultStmt*>(clauses[i].statement)) {
        for (const auto& nested : defaultStmt->statements) {
          lowerStatement(*nested);
        }
      }
      if (currentBlock_ != nullptr && !isCurrentBlockTerminated()) {
        emitJump(i + 1 < clauses.size() ? *clauses[i + 1].block : endBlock);
      }
    }
    breakTargets_.pop_back();
    currentBlock_ = &endBlock;
  }

  const TranslationUnit& unit_;
  compiler::ir::core::Module* module_ = nullptr;
  compiler::ir::builder::IRBuilder* builder_ = nullptr;
  compiler::ir::core::BasicBlock* currentBlock_ = nullptr;
  std::vector<std::unordered_map<std::string, VariableInfo>> scopes_;
  std::unordered_map<std::string, FunctionInfo> functions_;
  std::unordered_map<std::string, compiler::ir::core::BasicBlock*> labelBlocks_;
  std::vector<compiler::ir::core::BasicBlock*> breakTargets_;
  std::vector<compiler::ir::core::BasicBlock*> continueTargets_;
  std::string currentFunctionName_;
  LoweredType currentReturnType_;
  std::size_t tempCounter_ = 0;
  std::size_t blockCounter_ = 0;
};

}  // namespace

compiler::ir::core::Module AstToIR::lower(const compiler::frontend::ast::TranslationUnit& unit,
                                          const std::string& sourceName,
                                          const LoweringContext& context) const {
  return Lowerer(unit).run(sourceName, context);
}

}  // namespace compiler::ir::builder
