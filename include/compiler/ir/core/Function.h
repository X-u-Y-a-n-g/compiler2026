#pragma once

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "BasicBlock.h"

namespace compiler::ir::core {

struct FunctionParameter {
  std::string name;
  std::string type;
};

class Function {
 public:
  explicit Function(std::string name, std::string returnType = "void",
                    std::vector<FunctionParameter> parameters = {})
      : name_(std::move(name)),
        returnType_(std::move(returnType)),
        parameters_(std::move(parameters)) {}

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::string& returnType() const { return returnType_; }
  [[nodiscard]] const std::vector<FunctionParameter>& parameters() const { return parameters_; }
  [[nodiscard]] std::vector<FunctionParameter>& parameters() { return parameters_; }
  [[nodiscard]] const std::deque<BasicBlock>& blocks() const { return blocks_; }
  [[nodiscard]] std::deque<BasicBlock>& blocks() { return blocks_; }
  [[nodiscard]] bool hasIterationScalarResidency() const {
    return hasIterationScalarResidency_;
  }
  void setIterationScalarResidency(bool value) {
    hasIterationScalarResidency_ = value;
  }

  BasicBlock& addBlock(BasicBlock block) {
    blocks_.push_back(std::move(block));
    return blocks_.back();
  }

 private:
  std::string name_;
  std::string returnType_;
  std::vector<FunctionParameter> parameters_;
  std::deque<BasicBlock> blocks_;
  bool hasIterationScalarResidency_ = false;
};

}  // namespace compiler::ir::core
