#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace compiler::ir::core {
class Module;
}

namespace compiler::midend::analysis {

struct FunctionEffects {
  bool readsExternalMemory = false;
  bool writesExternalMemory = false;
  bool hasOtherSideEffects = false;
  bool readsUnknownMemory = false;
  bool writesUnknownMemory = false;
  std::unordered_set<std::string> readSymbols;
  std::unordered_set<std::string> writtenSymbols;

  [[nodiscard]] bool isReadNone() const {
    return !readsExternalMemory && !writesExternalMemory &&
           !hasOtherSideEffects;
  }

  [[nodiscard]] bool isReadOnly() const {
    return readsExternalMemory && !writesExternalMemory &&
           !hasOtherSideEffects;
  }

  bool merge(const FunctionEffects& other);

  [[nodiscard]] bool mayReadSymbol(std::string_view symbol) const {
    return readsUnknownMemory ||
           readSymbols.find(std::string(symbol)) != readSymbols.end();
  }

  [[nodiscard]] bool mayWriteSymbol(std::string_view symbol) const {
    return writesUnknownMemory ||
           writtenSymbols.find(std::string(symbol)) != writtenSymbols.end();
  }
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
