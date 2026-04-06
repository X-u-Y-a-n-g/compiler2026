#pragma once

#include <string>
#include <utility>
#include <vector>

#include "compiler/ir/core/Value.h"

namespace compiler::ir::core {

class Instruction : public Value {
 public:
  Instruction(std::string opcode, std::vector<std::string> operands = {})
      : opcode_(std::move(opcode)), operands_(std::move(operands)) {}

  [[nodiscard]] const std::string& opcode() const { return opcode_; }
  [[nodiscard]] const std::vector<std::string>& operands() const { return operands_; }

 private:
  std::string opcode_;
  std::vector<std::string> operands_;
};

}  // namespace compiler::ir::core
