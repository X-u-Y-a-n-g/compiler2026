#pragma once

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace compiler::backend::target {

class TargetInfo {
 public:
  static TargetInfo fromName(const std::string& name);

  TargetInfo(std::string name, std::string triple, unsigned pointerSize, unsigned stackAlignment,
             std::vector<std::string> integerArgRegisters,
             std::vector<std::string> floatArgRegisters,
             std::vector<std::string> calleeSavedIntegerRegisters,
             std::vector<std::string> allocatableIntegerRegisters,
             std::vector<std::string> scratchIntegerRegisters,
             std::vector<std::string> scratchFloatRegisters)
      : name_(std::move(name)),
        triple_(std::move(triple)),
        pointerSize_(pointerSize),
        stackAlignment_(stackAlignment),
        integerArgRegisters_(std::move(integerArgRegisters)),
        floatArgRegisters_(std::move(floatArgRegisters)),
        calleeSavedIntegerRegisters_(std::move(calleeSavedIntegerRegisters)),
        allocatableIntegerRegisters_(std::move(allocatableIntegerRegisters)),
        scratchIntegerRegisters_(std::move(scratchIntegerRegisters)),
        scratchFloatRegisters_(std::move(scratchFloatRegisters)) {}

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::string& triple() const { return triple_; }
  [[nodiscard]] unsigned pointerSize() const { return pointerSize_; }
  [[nodiscard]] unsigned stackAlignment() const { return stackAlignment_; }
  [[nodiscard]] const std::vector<std::string>& integerArgRegisters() const {
    return integerArgRegisters_;
  }
  [[nodiscard]] const std::vector<std::string>& floatArgRegisters() const {
    return floatArgRegisters_;
  }
  [[nodiscard]] const std::vector<std::string>& calleeSavedIntegerRegisters() const {
    return calleeSavedIntegerRegisters_;
  }
  [[nodiscard]] const std::vector<std::string>& allocatableIntegerRegisters() const {
    return allocatableIntegerRegisters_;
  }
  [[nodiscard]] const std::vector<std::string>& scratchIntegerRegisters() const {
    return scratchIntegerRegisters_;
  }
  [[nodiscard]] const std::vector<std::string>& scratchFloatRegisters() const {
    return scratchFloatRegisters_;
  }

 private:
  std::string name_;
  std::string triple_;
  unsigned pointerSize_ = 8;
  unsigned stackAlignment_ = 16;
  std::vector<std::string> integerArgRegisters_;
  std::vector<std::string> floatArgRegisters_;
  std::vector<std::string> calleeSavedIntegerRegisters_;
  std::vector<std::string> allocatableIntegerRegisters_;
  std::vector<std::string> scratchIntegerRegisters_;
  std::vector<std::string> scratchFloatRegisters_;
};

}  // namespace compiler::backend::target
