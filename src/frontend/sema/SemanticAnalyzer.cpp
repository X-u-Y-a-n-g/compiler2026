#include "compiler/frontend/sema/SemanticAnalyzer.h"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compiler/frontend/types/Type.h"

namespace compiler::frontend::sema {

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
using compiler::frontend::ast::InitListExpr;
using compiler::frontend::ast::IndexExpr;
using compiler::frontend::ast::IntegerLiteral;
using compiler::frontend::ast::LabelStmt;
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
using compiler::support::DiagnosticEngine;
using compiler::support::SourceLocation;

enum class SymbolKind {
  Variable,
  Function,
};

enum class ExprResult {
  Value,
  VoidValue,
};

struct ParameterShape {
  TypeKind type = TypeKind::Unknown;
  std::size_t arrayRank = 0;
  std::vector<std::optional<long long>> dimensions;
};

struct FunctionInfo {
  SourceLocation location;
  TypeKind returnType = TypeKind::Unknown;
  std::vector<ParameterShape> parameters;
  bool hasDefinition = false;
  bool isBuiltin = false;
};

struct SymbolInfo {
  SymbolKind kind = SymbolKind::Variable;
  SourceLocation location;
  TypeKind type = TypeKind::Unknown;
  bool isConst = false;
  bool isArray = false;
  std::size_t arrayRank = 0;
  std::vector<std::optional<long long>> arrayDimensions;
  bool isGlobal = false;
  bool isConstantExpression = false;
  std::optional<long long> integerConstantValue;
};

struct GotoInfo {
  SourceLocation location;
  std::string label;
};

struct SwitchContext {
  bool sawDefault = false;
  std::unordered_map<long long, SourceLocation> caseValues;
};

struct FunctionContext {
  std::string name;
  TypeKind returnType = TypeKind::Unknown;
  int loopDepth = 0;
  std::vector<SwitchContext> switches;
  std::vector<GotoInfo> gotos;
  std::unordered_map<std::string, SourceLocation> labels;
};

class Analyzer {
 public:
  Analyzer(const TranslationUnit& unit, DiagnosticEngine& diagnostics)
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

  static bool isArrayLike(const VariableDecl& decl) {
    return !decl.dimensions.empty();
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

  static std::vector<std::optional<long long>> buildDeclaredArrayDimensions(
      const VariableDecl& variable) {
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

  static ParameterShape buildParameterShape(const VariableDecl& parameter) {
    ParameterShape shape;
    shape.type = parameter.type.type.kind;
    shape.arrayRank = parameter.dimensions.size();
    shape.dimensions = buildDeclaredArrayDimensions(parameter);
    return shape;
  }

  static std::vector<ParameterShape> buildParameterShapes(const FunctionDecl& function) {
    std::vector<ParameterShape> shapes;
    shapes.reserve(function.parameters.size());
    for (const auto& parameter : function.parameters) {
      shapes.push_back(buildParameterShape(*parameter));
    }
    return shapes;
  }

  static bool haveCompatibleParameterShape(const ParameterShape& lhs, const ParameterShape& rhs) {
    if (lhs.type != rhs.type || lhs.arrayRank != rhs.arrayRank) {
      return false;
    }

    if (lhs.arrayRank == 0) {
      return true;
    }

    const auto sharedDims = std::min(lhs.dimensions.size(), rhs.dimensions.size());
    for (std::size_t i = 1; i < sharedDims; ++i) {
      if (lhs.dimensions[i].has_value() && rhs.dimensions[i].has_value() &&
          lhs.dimensions[i] != rhs.dimensions[i]) {
        return false;
      }
    }
    return true;
  }

  static ParameterShape scalarParameter(TypeKind type) {
    ParameterShape shape;
    shape.type = type;
    return shape;
  }

  static ParameterShape arrayParameter(TypeKind type, std::size_t rank = 1) {
    ParameterShape shape;
    shape.type = type;
    shape.arrayRank = rank;
    shape.dimensions.assign(rank, std::nullopt);
    return shape;
  }

  void notePreviousDeclaration(const SourceLocation& location) {
    diagnostics_.note(location, "previous declaration is here");
  }

  void reportRedefinition(const std::string& name, const SourceLocation& location,
                          const SourceLocation& previous) {
    diagnostics_.error(location, "redefinition of '" + name + "'");
    notePreviousDeclaration(previous);
  }

  bool declareInCurrentScope(const std::string& name, SymbolKind kind,
                             const SourceLocation& location) {
    SymbolInfo symbol;
    symbol.kind = kind;
    symbol.location = location;
    return declareInCurrentScope(name, symbol);
  }

  bool declareInCurrentScope(const std::string& name, const SymbolInfo& symbol) {
    if (name.empty()) {
      return false;
    }

    auto& scope = scopes_.back();
    const auto it = scope.find(name);
    if (it != scope.end()) {
      reportRedefinition(name, symbol.location, it->second.location);
      return false;
    }

    scope.emplace(name, symbol);
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

  static bool isIntegralType(TypeKind kind) {
    return kind == TypeKind::Int || kind == TypeKind::Long || kind == TypeKind::Bool;
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

  static SymbolInfo makeVariableSymbol(const VariableDecl& variable, bool isGlobal) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Variable;
    symbol.location = variable.location();
    symbol.type = variable.type.type.kind;
    symbol.isConst = variable.type.isConst;
    symbol.isArray = isArrayLike(variable);
    symbol.arrayRank = variable.dimensions.size();
    symbol.arrayDimensions = buildDeclaredArrayDimensions(variable);
    symbol.isGlobal = isGlobal;
    return symbol;
  }

  static SymbolInfo makeFunctionSymbol(const SourceLocation& location) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Function;
    symbol.location = location;
    return symbol;
  }

  void registerBuiltinFunction(const std::string& name, TypeKind returnType,
                               std::vector<ParameterShape> parameters) {
    functions_.emplace(name, FunctionInfo{{}, returnType, std::move(parameters), true, true});
    scopes_.front().emplace(name, makeFunctionSymbol({}));
  }

  void registerBuiltins() {
    registerBuiltinFunction("getint", TypeKind::Int, {});
    registerBuiltinFunction("getch", TypeKind::Int, {});
    registerBuiltinFunction("getfloat", TypeKind::Float, {});
    registerBuiltinFunction("getarray", TypeKind::Int, {arrayParameter(TypeKind::Int)});
    registerBuiltinFunction("getfarray", TypeKind::Int, {arrayParameter(TypeKind::Float)});
    registerBuiltinFunction("putint", TypeKind::Void, {scalarParameter(TypeKind::Int)});
    registerBuiltinFunction("putch", TypeKind::Void, {scalarParameter(TypeKind::Int)});
    registerBuiltinFunction("putfloat", TypeKind::Void, {scalarParameter(TypeKind::Float)});
    registerBuiltinFunction("putarray", TypeKind::Void,
                            {scalarParameter(TypeKind::Int), arrayParameter(TypeKind::Int)});
    registerBuiltinFunction("putfarray", TypeKind::Void,
                            {scalarParameter(TypeKind::Int), arrayParameter(TypeKind::Float)});
    registerBuiltinFunction("starttime", TypeKind::Void, {});
    registerBuiltinFunction("stoptime", TypeKind::Void, {});
  }

  void collectGlobalDeclarations() {
    for (const auto& declaration : unit_.declarations) {
      if (const auto* function = dynamic_cast<const FunctionDecl*>(declaration.get())) {
        collectFunctionDeclaration(*function);
        continue;
      }
      if (const auto* group = dynamic_cast<const VariableDeclGroup*>(declaration.get())) {
        collectGlobalVariableGroup(*group);
      }
    }
  }

  void collectGlobalVariableGroup(const VariableDeclGroup& group) {
    for (const auto& variable : group.declarations) {
      if (const auto found = scopes_.front().find(variable->name); found != scopes_.front().end()) {
        reportRedefinition(variable->name, variable->location(), found->second.location);
        continue;
      }
      scopes_.front().emplace(variable->name, makeVariableSymbol(*variable, true));
    }
  }

  void collectFunctionDeclaration(const FunctionDecl& function) {
    const auto newParameters = buildParameterShapes(function);
    const auto found = functions_.find(function.name);
    if (found == functions_.end()) {
      functions_.emplace(function.name,
                         FunctionInfo{function.location(), function.returnType.type.kind,
                                      newParameters, function.body != nullptr, false});
      scopes_.front().emplace(function.name, makeFunctionSymbol(function.location()));
      return;
    }

    auto& existing = found->second;
    if (existing.returnType != function.returnType.type.kind ||
        existing.parameters.size() != newParameters.size()) {
      diagnostics_.error(function.location(),
                         "conflicting declaration of function '" + function.name + "'");
      notePreviousDeclaration(existing.location);
      return;
    }

    for (std::size_t i = 0; i < newParameters.size(); ++i) {
      if (!haveCompatibleParameterShape(existing.parameters[i], newParameters[i])) {
        diagnostics_.error(function.location(),
                           "conflicting declaration of function '" + function.name + "'");
        notePreviousDeclaration(existing.location);
        return;
      }
    }

    if (existing.isBuiltin && function.body != nullptr) {
      diagnostics_.error(function.location(),
                         "cannot provide a definition for builtin function '" + function.name +
                             "'");
      notePreviousDeclaration(existing.location);
      return;
    }

    if (existing.hasDefinition && function.body != nullptr) {
      diagnostics_.error(function.location(),
                         "redefinition of function '" + function.name + "'");
      notePreviousDeclaration(existing.location);
      return;
    }

    if (!existing.isBuiltin && existing.location.offset == 0 && existing.location.line == 1 &&
        existing.location.column == 1 && function.location().offset == 0) {
      existing.location = function.location();
    }
    if (function.body != nullptr) {
      existing.hasDefinition = true;
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
    currentFunction_.emplace();
    currentFunction_->name = function.name;
    currentFunction_->returnType = function.returnType.type.kind;

    for (const auto& parameter : function.parameters) {
      analyzeParameter(*parameter, function.body != nullptr);
    }

    if (function.body) {
      analyzeStatement(*function.body);
      resolveGotos();
    }

    currentFunction_.reset();
    popScope();
  }

  void analyzeParameter(const VariableDecl& parameter, bool inDefinition) {
    checkVariableType(parameter, true);
    checkArrayDimensions(parameter, true, false);

    if (parameter.name.empty()) {
      if (inDefinition) {
        diagnostics_.error(parameter.location(),
                           "function definition parameters must have a name");
      }
      return;
    }

    declareInCurrentScope(parameter.name, makeVariableSymbol(parameter, false));
  }

  void analyzeVariableGroup(const VariableDeclGroup& group, bool declareLocals) {
    for (const auto& variable : group.declarations) {
      analyzeVariable(*variable, declareLocals);
    }
  }

  void checkVariableType(const VariableDecl& variable, bool isParameter) {
    if (variable.type.type.kind == TypeKind::Void) {
      diagnostics_.error(variable.location(),
                         std::string(isParameter ? "parameter" : "variable") +
                             " '" + variable.name + "' cannot have type void");
    }
  }

  void analyzeVariable(const VariableDecl& variable, bool declareLocals) {
    checkVariableType(variable, variable.isParameter);
    checkArrayDimensions(variable, variable.isParameter, !declareLocals);

    if (variable.type.isConst && !variable.initializer) {
      diagnostics_.error(variable.location(),
                         "const variable '" + variable.name + "' must have an initializer");
    }

    bool initializerIsConstExpr = false;
    std::optional<long long> initializerIntValue;
    if (variable.initializer) {
      analyzeInitializer(*variable.initializer);
      initializerIsConstExpr = isConstantInitializer(*variable.initializer);
      initializerIntValue = tryEvaluateScalarIntegerInitializer(*variable.initializer);
    }

    if (variable.type.isConst && variable.initializer && !initializerIsConstExpr) {
      diagnostics_.error(variable.initializer->location(),
                         "const variable '" + variable.name +
                             "' must be initialized with a constant expression");
    }

    if (!declareLocals && variable.initializer && !initializerIsConstExpr) {
      diagnostics_.error(variable.initializer->location(),
                         "global initializer for '" + variable.name +
                             "' must be a constant expression");
    }

    if (!declareLocals && variable.initializer == nullptr && variable.type.isConst) {
      return;
    }

    SymbolInfo symbol = makeVariableSymbol(variable, !declareLocals);
    symbol.isConstantExpression = variable.type.isConst && initializerIsConstExpr;
    if (symbol.isConstantExpression && initializerIntValue.has_value() &&
        isIntegralType(variable.type.type.kind)) {
      symbol.integerConstantValue = initializerIntValue;
    }

    if (declareLocals && !variable.name.empty()) {
      declareInCurrentScope(variable.name, symbol);
      return;
    }

    if (auto* existing = lookupSymbolMutable(variable.name); existing != nullptr &&
        existing->kind == SymbolKind::Variable) {
      existing->type = symbol.type;
      existing->isConst = symbol.isConst;
      existing->isArray = symbol.isArray;
      existing->arrayRank = symbol.arrayRank;
      existing->arrayDimensions = symbol.arrayDimensions;
      existing->isGlobal = symbol.isGlobal;
      existing->isConstantExpression = symbol.isConstantExpression;
      existing->integerConstantValue = symbol.integerConstantValue;
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
        analyzeExpression(*exprStmt->expression, nullptr);
      }
      return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
      analyzeReturnStatement(*returnStmt);
      return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
      analyzeExpression(*ifStmt->condition, "condition expression must produce a value");
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
        analyzeExpression(*forStmt->condition, "condition expression must produce a value");
      }
      if (forStmt->update) {
        analyzeExpression(*forStmt->update, nullptr);
      }
      if (currentFunction_) {
        ++currentFunction_->loopDepth;
      }
      if (forStmt->body) {
        analyzeStatement(*forStmt->body);
      }
      if (currentFunction_) {
        --currentFunction_->loopDepth;
      }
      popScope();
      return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
      analyzeExpression(*whileStmt->condition, "condition expression must produce a value");
      if (currentFunction_) {
        ++currentFunction_->loopDepth;
      }
      if (whileStmt->body) {
        analyzeStatement(*whileStmt->body);
      }
      if (currentFunction_) {
        --currentFunction_->loopDepth;
      }
      return;
    }

    if (const auto* doWhileStmt = dynamic_cast<const DoWhileStmt*>(&statement)) {
      if (currentFunction_) {
        ++currentFunction_->loopDepth;
      }
      if (doWhileStmt->body) {
        analyzeStatement(*doWhileStmt->body);
      }
      if (currentFunction_) {
        --currentFunction_->loopDepth;
      }
      analyzeExpression(*doWhileStmt->condition, "condition expression must produce a value");
      return;
    }

    if (const auto* switchStmt = dynamic_cast<const SwitchStmt*>(&statement)) {
      analyzeExpression(*switchStmt->condition, "condition expression must produce a value");
      if (currentFunction_) {
        currentFunction_->switches.push_back({});
      }
      for (const auto& clause : switchStmt->clauses) {
        analyzeStatement(*clause);
      }
      if (currentFunction_) {
        currentFunction_->switches.pop_back();
      }
      return;
    }

    if (const auto* caseStmt = dynamic_cast<const CaseStmt*>(&statement)) {
      if (!currentFunction_ || currentFunction_->switches.empty()) {
        diagnostics_.error(caseStmt->location(), "'case' statement is only valid inside switch");
      }
      analyzeExpression(*caseStmt->value, "case expression must produce a value");
      const auto caseValue = tryEvaluateIntegerConstant(*caseStmt->value);
      if (!caseValue.has_value()) {
        diagnostics_.error(caseStmt->value->location(),
                           "case label must be an integer constant expression");
      } else if (currentFunction_ && !currentFunction_->switches.empty()) {
        auto& currentSwitch = currentFunction_->switches.back();
        const auto found = currentSwitch.caseValues.find(*caseValue);
        if (found != currentSwitch.caseValues.end()) {
          diagnostics_.error(caseStmt->location(),
                             "duplicate case value in the same switch statement");
          notePreviousDeclaration(found->second);
        } else {
          currentSwitch.caseValues.emplace(*caseValue, caseStmt->location());
        }
      }
      for (const auto& nested : caseStmt->statements) {
        analyzeStatement(*nested);
      }
      return;
    }

    if (const auto* defaultStmt = dynamic_cast<const DefaultStmt*>(&statement)) {
      if (!currentFunction_ || currentFunction_->switches.empty()) {
        diagnostics_.error(defaultStmt->location(),
                           "'default' statement is only valid inside switch");
      } else if (currentFunction_->switches.back().sawDefault) {
        diagnostics_.error(defaultStmt->location(),
                           "duplicate 'default' label in the same switch statement");
      } else {
        currentFunction_->switches.back().sawDefault = true;
      }
      for (const auto& nested : defaultStmt->statements) {
        analyzeStatement(*nested);
      }
      return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&statement)) {
      if (!currentFunction_ ||
          (currentFunction_->loopDepth == 0 && currentFunction_->switches.empty())) {
        diagnostics_.error(breakStmt->location(), "'break' is only valid inside loop or switch");
      }
      return;
    }

    if (const auto* continueStmt = dynamic_cast<const ContinueStmt*>(&statement)) {
      if (!currentFunction_ || currentFunction_->loopDepth == 0) {
        diagnostics_.error(continueStmt->location(), "'continue' is only valid inside loop");
      }
      return;
    }

    if (const auto* gotoStmt = dynamic_cast<const GotoStmt*>(&statement)) {
      if (currentFunction_) {
        currentFunction_->gotos.push_back({gotoStmt->location(), gotoStmt->label});
      }
      return;
    }

    if (const auto* labelStmt = dynamic_cast<const LabelStmt*>(&statement)) {
      if (currentFunction_) {
        const auto found = currentFunction_->labels.find(labelStmt->label);
        if (found != currentFunction_->labels.end()) {
          reportRedefinition(labelStmt->label, labelStmt->location(), found->second);
        } else {
          currentFunction_->labels.emplace(labelStmt->label, labelStmt->location());
        }
      }
      if (labelStmt->statement) {
        analyzeStatement(*labelStmt->statement);
      }
    }
  }

  void analyzeReturnStatement(const ReturnStmt& statement) {
    if (!currentFunction_) {
      return;
    }

    if (currentFunction_->returnType == TypeKind::Void) {
      if (statement.expression) {
        diagnostics_.error(statement.location(),
                           "void function '" + currentFunction_->name +
                               "' must not return a value");
        analyzeExpression(*statement.expression, nullptr);
      }
      return;
    }

    if (!statement.expression) {
      diagnostics_.error(statement.location(),
                         "non-void function '" + currentFunction_->name +
                             "' must return a value");
      return;
    }

    analyzeExpression(*statement.expression, "return expression must produce a value");
  }

  static bool isAssignableExpression(const Expr& expression) {
    return dynamic_cast<const IdentifierExpr*>(&expression) != nullptr ||
           dynamic_cast<const IndexExpr*>(&expression) != nullptr;
  }

  std::size_t inferExpressionArrayRank(const Expr& expression) const {
    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* symbol = lookupSymbol(identifier->name);
      if (symbol == nullptr || symbol->kind != SymbolKind::Variable) {
        return 0;
      }
      return symbol->arrayRank;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
      const auto baseRank = inferExpressionArrayRank(*index->base);
      return baseRank == 0 ? 0 : baseRank - 1;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Comma) {
        return inferExpressionArrayRank(*binary->rhs);
      }
      return 0;
    }

    return 0;
  }

  ExprResult analyzeExpression(const Expr& expression, const char* voidUseMessage) {
    ExprResult result = ExprResult::Value;

    if (const auto* initList = dynamic_cast<const InitListExpr*>(&expression)) {
      for (const auto& element : initList->elements) {
        analyzeInitializer(*element);
      }
      if (voidUseMessage != nullptr) {
        diagnostics_.error(initList->location(), voidUseMessage);
      }
      return ExprResult::Value;
    }

    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* symbol = lookupSymbol(identifier->name);
      if (!symbol) {
        diagnostics_.error(identifier->location(),
                           "use of undeclared identifier '" + identifier->name + "'");
        return ExprResult::Value;
      }
      if (symbol->kind == SymbolKind::Function) {
        diagnostics_.error(identifier->location(),
                           "function '" + identifier->name + "' cannot be used as a value");
      }
      return ExprResult::Value;
    }

    if (dynamic_cast<const IntegerLiteral*>(&expression) != nullptr ||
        dynamic_cast<const FloatLiteralExpr*>(&expression) != nullptr ||
        dynamic_cast<const StringLiteralExpr*>(&expression) != nullptr) {
      return ExprResult::Value;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      analyzeExpression(*unary->operand, "operator operand must produce a value");
      return ExprResult::Value;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Assign) {
        analyzeExpression(*binary->lhs, nullptr);
        if (!isAssignableExpression(*binary->lhs)) {
          diagnostics_.error(binary->lhs->location(), "assignment target is not assignable");
        }
        analyzeExpression(*binary->rhs, "assignment right-hand side must produce a value");
        return ExprResult::Value;
      }

      if (binary->op == TokenKind::Comma) {
        analyzeExpression(*binary->lhs, nullptr);
        result = analyzeExpression(*binary->rhs, voidUseMessage);
        return result;
      }

      analyzeExpression(*binary->lhs, "operator operand must produce a value");
      analyzeExpression(*binary->rhs, "operator operand must produce a value");
      if ((binary->op == TokenKind::Slash || binary->op == TokenKind::Percent) &&
          tryEvaluateIntegerConstant(*binary->rhs) == std::optional<long long>{0}) {
        diagnostics_.error(binary->rhs->location(),
                           binary->op == TokenKind::Slash
                               ? "division by zero with constant zero divisor"
                               : "modulo by zero with constant zero divisor");
      }
      return ExprResult::Value;
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
      const FunctionInfo* function = nullptr;
      if (const auto* callee = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
        const auto* symbol = lookupSymbol(callee->name);
        if (!symbol) {
          diagnostics_.error(callee->location(),
                             "call to undeclared function '" + callee->name + "'");
        } else if (symbol->kind != SymbolKind::Function) {
          diagnostics_.error(callee->location(),
                             "'" + callee->name + "' is not a function");
        } else {
          function = lookupFunction(callee->name);
        }
      } else {
        diagnostics_.error(call->location(), "call target must be a function identifier");
        analyzeExpression(*call->callee, nullptr);
      }

      for (const auto& argument : call->arguments) {
        analyzeExpression(*argument, "function argument must produce a value");
      }

      if (function != nullptr && call->arguments.size() != function->parameters.size()) {
        diagnostics_.error(call->location(),
                           "function '" + functionName(*call) + "' expects " +
                               std::to_string(function->parameters.size()) + " argument(s), got " +
                               std::to_string(call->arguments.size()));
      } else if (function != nullptr) {
        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
          const auto argumentRank = inferExpressionArrayRank(*call->arguments[i]);
          const auto& parameter = function->parameters[i];
          if (parameter.arrayRank == 0 && argumentRank > 0) {
            diagnostics_.error(call->arguments[i]->location(),
                               "function '" + functionName(*call) + "' argument " +
                                   std::to_string(i + 1) + " expects a scalar value");
            continue;
          }
          if (parameter.arrayRank > 0 && argumentRank < parameter.arrayRank) {
            diagnostics_.error(call->arguments[i]->location(),
                               "function '" + functionName(*call) + "' argument " +
                                   std::to_string(i + 1) + " expects an array value with rank " +
                                   std::to_string(parameter.arrayRank));
          }
        }
      }

      if (function != nullptr && function->returnType == TypeKind::Void) {
        result = ExprResult::VoidValue;
      }
      if (result == ExprResult::VoidValue && voidUseMessage != nullptr) {
        diagnostics_.error(call->location(), voidUseMessage);
      }
      return result;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
      analyzeExpression(*index->base, "subscript base must produce a value");
      analyzeExpression(*index->index, "array index must produce a value");
      return ExprResult::Value;
    }

    if (result == ExprResult::VoidValue && voidUseMessage != nullptr) {
      diagnostics_.error(expression.location(), voidUseMessage);
    }
    return result;
  }

  static std::string functionName(const CallExpr& call) {
    if (const auto* callee = dynamic_cast<const IdentifierExpr*>(call.callee.get())) {
      return callee->name;
    }
    return "<call>";
  }

  void analyzeInitializer(const Expr& initializer) {
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&initializer)) {
      for (const auto& element : initList->elements) {
        analyzeInitializer(*element);
      }
      return;
    }
    analyzeExpression(initializer, "initializer expression must produce a value");
  }

  bool isConstantExpression(const Expr& expression) const {
    if (dynamic_cast<const IntegerLiteral*>(&expression) != nullptr ||
        dynamic_cast<const FloatLiteralExpr*>(&expression) != nullptr ||
        dynamic_cast<const StringLiteralExpr*>(&expression) != nullptr) {
      return true;
    }

    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(&expression)) {
      const auto* symbol = lookupSymbol(identifier->name);
      return symbol != nullptr && symbol->kind == SymbolKind::Variable &&
             symbol->isConst && symbol->isConstantExpression;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
      return isConstantExpression(*unary->operand);
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
      if (binary->op == TokenKind::Assign || binary->op == TokenKind::Comma) {
        return false;
      }
      return isConstantExpression(*binary->lhs) && isConstantExpression(*binary->rhs);
    }

    return false;
  }

  bool isConstantInitializer(const Expr& initializer) const {
    if (const auto* initList = dynamic_cast<const InitListExpr*>(&initializer)) {
      for (const auto& element : initList->elements) {
        if (!isConstantInitializer(*element)) {
          return false;
        }
      }
      return true;
    }
    return isConstantExpression(initializer);
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
      if (symbol == nullptr || symbol->kind != SymbolKind::Variable || !symbol->isConst ||
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
      if (binary->op == TokenKind::Comma || binary->op == TokenKind::Assign) {
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

  void checkArrayDimensions(const VariableDecl& variable, bool isParameter,
                            bool /*isGlobalVariable*/) {
    for (std::size_t i = 0; i < variable.dimensions.size(); ++i) {
      const auto& dimension = variable.dimensions[i];
      if (!dimension) {
        if (!isParameter || i != 0) {
          diagnostics_.error(variable.location(),
                             "array dimension for '" + variable.name + "' must be specified");
        }
        continue;
      }

      analyzeExpression(*dimension, "array dimension must produce a value");
      const auto dimensionValue = tryEvaluateIntegerConstant(*dimension);
      if (!dimensionValue.has_value()) {
        diagnostics_.error(dimension->location(),
                           "array dimension for '" + variable.name +
                               "' must be an integer constant expression");
        continue;
      }
      if (*dimensionValue <= 0) {
        diagnostics_.error(dimension->location(),
                           "array dimension for '" + variable.name +
                               "' must be greater than zero");
      }
    }
  }

  void resolveGotos() {
    if (!currentFunction_) {
      return;
    }

    for (const auto& gotoInfo : currentFunction_->gotos) {
      if (currentFunction_->labels.find(gotoInfo.label) == currentFunction_->labels.end()) {
        diagnostics_.error(gotoInfo.location,
                           "goto references undefined label '" + gotoInfo.label + "'");
      }
    }
  }

  const TranslationUnit& unit_;
  DiagnosticEngine& diagnostics_;
  std::vector<std::unordered_map<std::string, SymbolInfo>> scopes_;
  std::unordered_map<std::string, FunctionInfo> functions_;
  std::optional<FunctionContext> currentFunction_;
};

}  // namespace

bool SemanticAnalyzer::analyze(const compiler::frontend::ast::TranslationUnit& unit,
                               compiler::support::DiagnosticEngine& diagnostics) const {
  return Analyzer(unit, diagnostics).run();
}

}  // namespace compiler::frontend::sema
