#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace compiler::ir::core {
class Function;
class Module;
}

namespace compiler::ir::analysis {

enum class AliasResult { MustAlias, NoAlias, MayAlias };
enum class PointerOrigin { Global, Stack, Parameter, Derived, Unknown };

struct PointerInfo {
  PointerOrigin origin = PointerOrigin::Unknown;
  std::string root;
  std::vector<std::string> indices;
  bool captured = true;
};

class AliasAnalysis {
 public:
  AliasAnalysis(const compiler::ir::core::Module& module,
                const compiler::ir::core::Function& function);

  [[nodiscard]] AliasResult alias(const std::string& lhs,
                                  const std::string& rhs) const;
  [[nodiscard]] PointerInfo pointerInfo(const std::string& value) const;
  [[nodiscard]] bool isCaptured(const std::string& value) const;

 private:
  std::unordered_map<std::string, PointerInfo> pointers_;
};

}  // namespace compiler::ir::analysis
