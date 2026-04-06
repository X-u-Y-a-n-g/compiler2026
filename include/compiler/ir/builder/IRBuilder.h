#pragma once

#include <string>
#include <utility>
#include <vector>

#include "compiler/ir/core/Function.h"

namespace compiler::ir::builder {

class IRBuilder {
 public:
  explicit IRBuilder(compiler::ir::core::Function& function) : function_(function) {}

  compiler::ir::core::BasicBlock& createBlock(const std::string& name) {
    return function_.addBlock(compiler::ir::core::BasicBlock{name});
  }

  void emit(compiler::ir::core::BasicBlock& block, std::string opcode,
            std::vector<std::string> operands = {}) {
    block.addInstruction(
        compiler::ir::core::Instruction{std::move(opcode), std::move(operands)});
  }

 private:
  compiler::ir::core::Function& function_;
};

}  // namespace compiler::ir::builder
