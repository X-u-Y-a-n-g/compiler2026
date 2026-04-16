#pragma once

#include <string>
#include <vector>

namespace compiler::backend::target {

class TargetRegInfo {
 public:
  virtual ~TargetRegInfo() = default;

  [[nodiscard]] virtual const std::string& stackPointerRegister() const = 0;
  [[nodiscard]] virtual const std::string& framePointerRegister() const = 0;
  [[nodiscard]] virtual const std::string& returnAddressRegister() const = 0;
  [[nodiscard]] virtual const std::string& zeroRegister() const = 0;

  [[nodiscard]] virtual const std::vector<std::string>& integerArgumentRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& floatArgumentRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& calleeSavedIntegerRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& calleeSavedFloatRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& reservedRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& allocatableIntegerRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& allocatableFloatRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& scratchIntegerRegisters() const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& scratchFloatRegisters() const = 0;
};

}  // namespace compiler::backend::target
