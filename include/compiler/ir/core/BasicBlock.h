#pragma once

#include <string>
#include <utility>
#include <vector>

#include "compiler/ir/core/Instruction.h"

namespace compiler::ir::core {

class BasicBlock {
 public:
  explicit BasicBlock(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::vector<Instruction>& instructions() const { return instructions_; }
  [[nodiscard]] std::vector<Instruction>& instructions() { return instructions_; }

  void addInstruction(Instruction instruction) { instructions_.push_back(std::move(instruction)); }

 private:
  std::string name_;
  std::vector<Instruction> instructions_;
};

}  // namespace compiler::ir::core
