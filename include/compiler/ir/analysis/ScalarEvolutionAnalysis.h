#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

#include "LoopAnalysis.h"

namespace compiler::ir::core {
class Function;
class Module;
}

namespace compiler::ir::analysis {

struct ScalarAddRecurrence {
  std::string value;
  std::string type;
  std::string start;
  long long step = 0;
  std::size_t loopHeader = 0;
};

struct TripCount {
  std::string start;
  std::string bound;
  long long step = 0;
  std::string predicate;
  std::optional<unsigned long long> exact;

  [[nodiscard]] bool isExact() const { return exact.has_value(); }
};

struct AffineAddress {
  std::string value;
  std::string base;
  std::string index;
  std::string start;
  long long stride = 0;
  std::size_t loopHeader = 0;
  bool linear = false;
};

class ScalarEvolutionAnalysis {
 public:
  ScalarEvolutionAnalysis(const compiler::ir::core::Module& module,
                          const compiler::ir::core::Function& function,
                          const LoopAnalysis& loops);

  [[nodiscard]] std::optional<ScalarAddRecurrence> recurrence(
      const std::string& value) const;
  [[nodiscard]] std::optional<TripCount> tripCount(
      std::size_t loopHeader) const;
  [[nodiscard]] std::optional<AffineAddress> affineAddress(
      const std::string& value) const;

 private:
  std::unordered_map<std::string, ScalarAddRecurrence> recurrences_;
  std::unordered_map<std::size_t, TripCount> tripCounts_;
  std::unordered_map<std::string, AffineAddress> addresses_;
};

}  // namespace compiler::ir::analysis
