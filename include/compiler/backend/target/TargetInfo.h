#pragma once

#include <string>
#include <utility>

namespace compiler::backend::target {

class TargetInfo {
 public:
  static TargetInfo fromName(const std::string& name);

  TargetInfo(std::string name, std::string triple, unsigned pointerSize, unsigned stackAlignment)
      : name_(std::move(name)),
        triple_(std::move(triple)),
        pointerSize_(pointerSize),
        stackAlignment_(stackAlignment) {}

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::string& triple() const { return triple_; }
  [[nodiscard]] unsigned pointerSize() const { return pointerSize_; }
  [[nodiscard]] unsigned stackAlignment() const { return stackAlignment_; }

 private:
  std::string name_;
  std::string triple_;
  unsigned pointerSize_ = 8;
  unsigned stackAlignment_ = 16;
};

}  // namespace compiler::backend::target
