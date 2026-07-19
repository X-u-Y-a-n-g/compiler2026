#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::ir::core {
class Function;
}

namespace compiler::ir::analysis {

struct CanonicalInductionVariable {
  std::string phi;
  std::string type;
  std::string initialValue;
  std::string updateValue;
  long long step = 0;
  std::string predicate;
  std::string bound;
};

struct NaturalLoop {
  std::size_t header = 0;
  std::unordered_set<std::size_t> blocks;
  std::vector<std::size_t> latches;
  std::vector<std::size_t> outsidePredecessors;
  std::vector<std::pair<std::size_t, std::size_t>> exitEdges;
  std::optional<std::size_t> preheader;
  bool hasUniqueLatch = false;
  bool hasDedicatedExits = false;
  bool isLoopClosedSSA = false;
  std::optional<CanonicalInductionVariable> induction;

  [[nodiscard]] bool contains(std::size_t block) const {
    return blocks.find(block) != blocks.end();
  }
};

class LoopAnalysis {
 public:
  explicit LoopAnalysis(const compiler::ir::core::Function& function);

  [[nodiscard]] const std::vector<NaturalLoop>& loops() const { return loops_; }
  [[nodiscard]] unsigned loopDepth(std::size_t block) const;
  [[nodiscard]] const std::vector<std::vector<std::size_t>>& predecessors() const {
    return predecessors_;
  }
  [[nodiscard]] const std::vector<std::vector<std::size_t>>& successors() const {
    return successors_;
  }
  [[nodiscard]] bool dominates(std::size_t dominator, std::size_t block) const;

 private:
  std::vector<NaturalLoop> loops_;
  std::vector<std::vector<std::size_t>> predecessors_;
  std::vector<std::vector<std::size_t>> successors_;
  std::vector<std::vector<bool>> dominators_;
};

}  // namespace compiler::ir::analysis
