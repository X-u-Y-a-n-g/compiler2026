#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace compiler::ir::core {
class Function;
class Instruction;
class Module;
}

namespace compiler::ir::analysis {

enum class ValueRefKind {
  SsaValue,
  GlobalAddress,
  IntegerLiteral,
  FloatingLiteral,
  BlockLabel,
  FunctionSymbol,
  TypeToken,
  Unknown,
};

struct TypedValueRef {
  ValueRefKind kind = ValueRefKind::Unknown;
  std::string spelling;
  std::string type;

  [[nodiscard]] bool isValue() const {
    return kind == ValueRefKind::SsaValue ||
           kind == ValueRefKind::GlobalAddress;
  }
};

struct DefinitionSite {
  std::string type;
  bool isParameter = false;
  bool isGlobal = false;
  std::size_t blockIndex = 0;
  std::size_t instructionIndex = 0;
};

struct UseSite {
  std::size_t blockIndex = 0;
  std::size_t instructionIndex = 0;
  std::size_t operandIndex = 0;
};

class UseDefAnalysis {
 public:
  UseDefAnalysis(const compiler::ir::core::Module& module,
                 const compiler::ir::core::Function& function);

  [[nodiscard]] std::optional<DefinitionSite> definition(
      std::string_view value) const;
  [[nodiscard]] const std::vector<UseSite>& uses(
      std::string_view value) const;
  [[nodiscard]] std::size_t useCount(std::string_view value) const;
  [[nodiscard]] TypedValueRef operand(
      const compiler::ir::core::Instruction& instruction,
      std::size_t operandIndex) const;
  [[nodiscard]] std::string typeOf(std::string_view value) const;

 private:
  std::unordered_map<std::string, DefinitionSite> definitions_;
  std::unordered_map<std::string, std::vector<UseSite>> uses_;
};

}  // namespace compiler::ir::analysis
