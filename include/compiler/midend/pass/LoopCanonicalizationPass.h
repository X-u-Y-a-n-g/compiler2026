#pragma once

#include <cstddef>
#include <cstdint>

#include "Pass.h"

namespace compiler::ir::core {
class Function;
}

namespace compiler::midend::pass {

enum class LoopCanonicalizationOption : std::uint8_t {
  None = 0,
  Preheaders = 1,
  Latches = 2,
  DedicatedExits = 4,
  LoopClosedSSA = 8,
  All = 15,
};

constexpr LoopCanonicalizationOption operator|(LoopCanonicalizationOption lhs,
                                                LoopCanonicalizationOption rhs) {
  return static_cast<LoopCanonicalizationOption>(
      static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] bool ensureLoopPreheader(
    compiler::ir::core::Function& function, std::size_t headerIndex);
[[nodiscard]] bool formLoopClosedSSA(
    const compiler::ir::core::Module& module,
    compiler::ir::core::Function& function, std::size_t headerIndex);

class LoopCanonicalizationPass final : public ModulePass {
 public:
  explicit LoopCanonicalizationPass(
      LoopCanonicalizationOption options = LoopCanonicalizationOption::All)
      : options_(options) {}

  std::string name() const override { return "loop-canonicalization"; }
  void run(compiler::ir::core::Module& module) override;

 private:
  LoopCanonicalizationOption options_;
};

}  // namespace compiler::midend::pass
