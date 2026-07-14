#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace compiler::ir::core {
class Module;
}

namespace compiler::midend::analysis {

struct FunctionEffects {
  bool readsExternalMemory = false;
  bool writesExternalMemory = false;
  bool hasOtherSideEffects = false;

  [[nodiscard]] bool isReadNone() const {
    return !readsExternalMemory && !writesExternalMemory &&
           !hasOtherSideEffects;
  }

  [[nodiscard]] bool isReadOnly() const {
    return readsExternalMemory && !writesExternalMemory &&
           !hasOtherSideEffects;
  }

  bool merge(const FunctionEffects& other);
};

class FunctionEffectAnalysis {
 public:
  explicit FunctionEffectAnalysis(const compiler::ir::core::Module& module);

  [[nodiscard]] const FunctionEffects& effectFor(
      std::string_view functionName) const;
  [[nodiscard]] bool isReadNone(std::string_view functionName) const;

 private:
  std::unordered_map<std::string, FunctionEffects> effects_;
};

}  // namespace compiler::midend::analysis
