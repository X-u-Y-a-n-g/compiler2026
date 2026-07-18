#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Function.h"
#include "GlobalVariable.h"

namespace compiler::ir::core {

class Module {
 public:
  explicit Module(std::string sourceName) : sourceName_(std::move(sourceName)) {}

  [[nodiscard]] const std::string& sourceName() const { return sourceName_; }
  [[nodiscard]] const std::vector<GlobalVariable>& globals() const { return globals_; }
  [[nodiscard]] std::vector<GlobalVariable>& globals() { return globals_; }
  [[nodiscard]] const std::vector<Function>& functions() const { return functions_; }
  [[nodiscard]] std::vector<Function>& functions() { return functions_; }
  [[nodiscard]] const std::vector<std::string>& passLog() const { return passLog_; }

  void addGlobal(GlobalVariable global) { globals_.push_back(std::move(global)); }

  Function& addFunction(Function function) {
    functions_.push_back(std::move(function));
    return functions_.back();
  }

  void addPassNote(std::string note) { passLog_.push_back(std::move(note)); }

 private:
  std::string sourceName_;
  std::vector<GlobalVariable> globals_;
  std::vector<Function> functions_;
  std::vector<std::string> passLog_;
};

}  // namespace compiler::ir::core
