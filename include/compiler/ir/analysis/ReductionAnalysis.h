#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "LoopAnalysis.h"

namespace compiler::ir::core {
class Function;
class Module;
}

namespace compiler::ir::analysis {

enum class ReductionKind { Add, Multiply, Xor, And, Or, Minimum, Maximum };

struct LoopReduction {
  ReductionKind kind = ReductionKind::Add;
  std::size_t loopHeader = 0;
  std::string type;
  std::string phi;
  std::string initialValue;
  std::string updateValue;
  std::string inputValue;
  bool floatingPoint = false;
  bool reassociationSafe = true;
};

class ReductionAnalysis {
 public:
  ReductionAnalysis(const compiler::ir::core::Module& module,
                    const compiler::ir::core::Function& function,
                    const LoopAnalysis& loops);

  [[nodiscard]] const std::vector<LoopReduction>& reductions() const {
    return reductions_;
  }
  [[nodiscard]] std::vector<LoopReduction> reductionsForLoop(
      std::size_t loopHeader) const;

 private:
  std::vector<LoopReduction> reductions_;
};

}  // namespace compiler::ir::analysis
