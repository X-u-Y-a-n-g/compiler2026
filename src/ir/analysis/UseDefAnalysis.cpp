#include "../../../include/compiler/ir/analysis/UseDefAnalysis.h"

#include <cerrno>
#include <cstdlib>
#include <string>

#include "../../../include/compiler/ir/core/Function.h"
#include "../../../include/compiler/ir/core/Instruction.h"
#include "../../../include/compiler/ir/core/Module.h"

namespace compiler::ir::analysis {
namespace {

using compiler::ir::core::Instruction;

enum class OperandRole { Value, Block, Function, Type };

OperandRole operandRole(const Instruction& instruction, std::size_t index) {
  const auto& opcode = instruction.opcode();
  const auto count = instruction.operands().size();
  if (opcode == "br") {
    if (count == 1 || index > 0) return OperandRole::Block;
    return OperandRole::Value;
  }
  if (opcode == "phi") {
    if (index == 0) return OperandRole::Type;
    return index % 2 == 0 ? OperandRole::Block : OperandRole::Value;
  }
  if (opcode == "call") return index == 0 ? OperandRole::Function : OperandRole::Value;
  if (opcode == "alloca") return OperandRole::Type;
  if (opcode == "load" || opcode == "store") {
    return index == 0 ? OperandRole::Type : OperandRole::Value;
  }
  if ((opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
       opcode == "sitofp" || opcode == "uitofp" || opcode == "fptosi" ||
       opcode == "cast") &&
      index == 1) {
    return OperandRole::Type;
  }
  return OperandRole::Value;
}

bool isIntegerLiteral(const std::string& text) {
  if (text == "true" || text == "false") return true;
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  std::strtoll(text.c_str(), &end, 0);
  return errno != ERANGE && end != text.c_str() && end != nullptr && *end == '\0';
}

bool isFloatingLiteral(const std::string& text) {
  if (text.find_first_of(".eEpP") == std::string::npos) return false;
  errno = 0;
  char* end = nullptr;
  std::strtod(text.c_str(), &end);
  return errno != ERANGE && end != text.c_str() && end != nullptr && *end == '\0';
}

std::string instructionResultType(const Instruction& instruction,
                                  const std::unordered_map<std::string,
                                                           DefinitionSite>& definitions,
                                  const std::unordered_map<std::string,
                                                           std::string>& returnTypes) {
  const auto& opcode = instruction.opcode();
  const auto& operands = instruction.operands();
  if ((opcode == "alloca") && !operands.empty()) return "ptr(" + operands[0] + ")";
  if ((opcode == "load" || opcode == "phi") && !operands.empty()) return operands[0];
  if (opcode.rfind("icmp.", 0) == 0 || opcode.rfind("fcmp.", 0) == 0) return "bool";
  if (opcode == "fneg" || opcode == "fadd" || opcode == "fsub" ||
      opcode == "fmul" || opcode == "fdiv" || opcode == "sitofp" ||
      opcode == "uitofp") {
    return "float";
  }
  if ((opcode == "zext" || opcode == "sext" || opcode == "trunc" ||
       opcode == "fptosi" || opcode == "cast") && operands.size() >= 2) {
    return operands[1];
  }
  if (opcode == "call" && !operands.empty() && operands[0].size() > 1 &&
      operands[0].front() == '@') {
    const auto found = returnTypes.find(operands[0].substr(1));
    return found == returnTypes.end() ? "int" : found->second;
  }
  for (const auto& operand : operands) {
    const auto found = definitions.find(operand);
    if (found != definitions.end() && !found->second.type.empty() &&
        found->second.type != "bool") {
      return found->second.type;
    }
  }
  return "int";
}

}  // namespace

UseDefAnalysis::UseDefAnalysis(const compiler::ir::core::Module& module,
                               const compiler::ir::core::Function& function) {
  std::unordered_map<std::string, std::string> returnTypes;
  for (const auto& candidate : module.functions()) {
    returnTypes.emplace(candidate.name(), candidate.returnType());
  }
  for (const auto& global : module.globals()) {
    definitions_.emplace("@" + global.name(),
                         DefinitionSite{"ptr(" + global.type() + ")", false,
                                        true, 0, 0});
  }
  for (const auto& parameter : function.parameters()) {
    definitions_.emplace(parameter.name,
                         DefinitionSite{parameter.type, true, false, 0, 0});
  }
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto& instruction = instructions[index];
      if (instruction.hasResult()) {
        definitions_[instruction.result()] =
            DefinitionSite{instructionResultType(instruction, definitions_,
                                                 returnTypes),
                           false, false, block, index};
      }
    }
  }
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const auto& instruction = instructions[index];
      for (std::size_t operandIndex = 0;
           operandIndex < instruction.operands().size(); ++operandIndex) {
        if (operandRole(instruction, operandIndex) != OperandRole::Value) continue;
        const auto& spelling = instruction.operands()[operandIndex];
        if (!spelling.empty() && (spelling.front() == '%' || spelling.front() == '@')) {
          uses_[spelling].push_back(UseSite{block, index, operandIndex});
        }
      }
    }
  }
}

std::optional<DefinitionSite> UseDefAnalysis::definition(
    std::string_view value) const {
  const auto found = definitions_.find(std::string(value));
  if (found == definitions_.end()) return std::nullopt;
  return found->second;
}

const std::vector<UseSite>& UseDefAnalysis::uses(std::string_view value) const {
  const auto found = uses_.find(std::string(value));
  if (found != uses_.end()) return found->second;
  static const std::vector<UseSite> empty;
  return empty;
}

std::size_t UseDefAnalysis::useCount(std::string_view value) const {
  return uses(value).size();
}

TypedValueRef UseDefAnalysis::operand(const Instruction& instruction,
                                      std::size_t operandIndex) const {
  if (operandIndex >= instruction.operands().size()) return {};
  const std::string& spelling = instruction.operands()[operandIndex];
  switch (operandRole(instruction, operandIndex)) {
    case OperandRole::Block:
      return {ValueRefKind::BlockLabel, spelling, "label"};
    case OperandRole::Function:
      return {ValueRefKind::FunctionSymbol, spelling, "function"};
    case OperandRole::Type:
      return {ValueRefKind::TypeToken, spelling, spelling};
    case OperandRole::Value:
      break;
  }
  if (!spelling.empty() && spelling.front() == '%') {
    return {ValueRefKind::SsaValue, spelling, typeOf(spelling)};
  }
  if (!spelling.empty() && spelling.front() == '@') {
    return {ValueRefKind::GlobalAddress, spelling, typeOf(spelling)};
  }
  if (isIntegerLiteral(spelling)) return {ValueRefKind::IntegerLiteral, spelling, "int"};
  if (isFloatingLiteral(spelling)) return {ValueRefKind::FloatingLiteral, spelling, "float"};
  return {ValueRefKind::Unknown, spelling, {}};
}

std::string UseDefAnalysis::typeOf(std::string_view value) const {
  const auto found = definitions_.find(std::string(value));
  return found == definitions_.end() ? std::string{} : found->second.type;
}

}  // namespace compiler::ir::analysis
