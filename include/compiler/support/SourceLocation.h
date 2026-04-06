#pragma once

#include <cstddef>
#include <string>

namespace compiler::support {

struct SourceLocation {
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;

  [[nodiscard]] std::string toString() const;
};

}  // namespace compiler::support
