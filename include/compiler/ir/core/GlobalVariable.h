#pragma once

#include <string>
#include <utility>

#include "compiler/ir/core/Constant.h"

namespace compiler::ir::core {

class GlobalVariable {
 public:
  GlobalVariable(std::string name, std::string type, Constant initializer, bool isConst = false)
      : name_(std::move(name)),
        type_(std::move(type)),
        initializer_(std::move(initializer)),
        isConst_(isConst) {}

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::string& type() const { return type_; }
  [[nodiscard]] const Constant& initializer() const { return initializer_; }
  [[nodiscard]] bool isConst() const { return isConst_; }

 private:
  std::string name_;
  std::string type_;
  Constant initializer_;
  bool isConst_ = false;
};

}  // namespace compiler::ir::core
