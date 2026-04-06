#pragma once

#include <string>
#include <utility>

namespace compiler::ir::core {

class Value {
 public:
  explicit Value(std::string name = "") : name_(std::move(name)) {}
  virtual ~Value() = default;

  [[nodiscard]] const std::string& name() const { return name_; }
  void setName(std::string name) { name_ = std::move(name); }

 private:
  std::string name_;
};

}  // namespace compiler::ir::core
