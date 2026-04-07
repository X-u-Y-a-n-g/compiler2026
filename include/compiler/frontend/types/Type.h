#pragma once

#include <string>

namespace compiler::frontend::types {

enum class TypeKind {
  Void,
  Int,
  Float,
  Bool,
  Long,
  Unknown,
};

struct Type {
  TypeKind kind = TypeKind::Unknown;

  [[nodiscard]] std::string toString() const;
};

}  // namespace compiler::frontend::types
