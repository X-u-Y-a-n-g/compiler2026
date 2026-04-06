#pragma once

#include <string>
#include <utility>
#include <vector>

#include "compiler/ir/core/BasicBlock.h"

namespace compiler::ir::core {

class Function {
 public:
  explicit Function(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::vector<BasicBlock>& blocks() const { return blocks_; }
  [[nodiscard]] std::vector<BasicBlock>& blocks() { return blocks_; }

  BasicBlock& addBlock(BasicBlock block) {
    blocks_.push_back(std::move(block));
    return blocks_.back();
  }

 private:
  std::string name_;
  std::vector<BasicBlock> blocks_;
};

}  // namespace compiler::ir::core
