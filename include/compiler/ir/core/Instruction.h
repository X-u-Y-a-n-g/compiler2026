#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Value.h"

namespace compiler::ir::core {

class Instruction : public Value {
 public:
  Instruction(std::string opcode, std::vector<std::string> operands = {}, std::string result = "")
      : Value(std::move(result)), opcode_(std::move(opcode)), operands_(std::move(operands)) {}

  [[nodiscard]] const std::string& opcode() const { return opcode_; }
  [[nodiscard]] const std::vector<std::string>& operands() const { return operands_; }
  [[nodiscard]] const std::string& result() const { return name(); }
  [[nodiscard]] bool hasResult() const { return !name().empty(); }
  [[nodiscard]] bool isTerminator() const { return opcode_ == "ret" || opcode_ == "br"; }

 private:
  std::string opcode_;
  std::vector<std::string> operands_;
};

}  // namespace compiler::ir::core
