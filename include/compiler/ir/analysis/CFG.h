#pragma once

#include <cstddef>

#include "Function.h"

namespace compiler::ir::analysis {

class CFG {
 public:
  explicit CFG(const compiler::ir::core::Function& function) : function_(function) {}

  [[nodiscard]] std::size_t blockCount() const { return function_.blocks().size(); }

 private:
  const compiler::ir::core::Function& function_;
};

}  // namespace compiler::ir::analysis
