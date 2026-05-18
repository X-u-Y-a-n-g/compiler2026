#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "SourceLocation.h"

namespace compiler::support {

enum class DiagnosticLevel {
  Note,
  Warning,
  Error,
};

struct Diagnostic {
  DiagnosticLevel level = DiagnosticLevel::Note;
  std::string message;
  SourceLocation location;
};

class DiagnosticEngine {
 public:
  void note(const SourceLocation& location, std::string message);
  void warning(const SourceLocation& location, std::string message);
  void error(const SourceLocation& location, std::string message);

  [[nodiscard]] bool hasErrors() const;
  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const;
  void print(std::ostream& os) const;

 private:
  void add(DiagnosticLevel level, const SourceLocation& location, std::string message);

  std::vector<Diagnostic> diagnostics_;
};

std::string toString(DiagnosticLevel level);

}  // namespace compiler::support
