#include "Diagnostic.h"

#include <iostream>
#include <sstream>
#include <utility>

namespace compiler::support {

std::string SourceLocation::toString() const {
  std::ostringstream os;
  os << line << ':' << column;
  return os.str();
}

void DiagnosticEngine::note(const SourceLocation& location, std::string message) {
  add(DiagnosticLevel::Note, location, std::move(message));
}

void DiagnosticEngine::warning(const SourceLocation& location, std::string message) {
  add(DiagnosticLevel::Warning, location, std::move(message));
}

void DiagnosticEngine::error(const SourceLocation& location, std::string message) {
  add(DiagnosticLevel::Error, location, std::move(message));
}

bool DiagnosticEngine::hasErrors() const {
  for (const auto& diagnostic : diagnostics_) {
    if (diagnostic.level == DiagnosticLevel::Error) {
      return true;
    }
  }
  return false;
}

const std::vector<Diagnostic>& DiagnosticEngine::diagnostics() const {
  return diagnostics_;
}

void DiagnosticEngine::print(std::ostream& os) const {
  for (const auto& diagnostic : diagnostics_) {
    os << toString(diagnostic.level) << " [" << diagnostic.location.toString() << "] "
       << diagnostic.message << '\n';
  }
}

void DiagnosticEngine::add(DiagnosticLevel level, const SourceLocation& location,
                           std::string message) {
  diagnostics_.push_back(Diagnostic{level, std::move(message), location});
}

std::string toString(DiagnosticLevel level) {
  switch (level) {
    case DiagnosticLevel::Note:
      return "note";
    case DiagnosticLevel::Warning:
      return "warning";
    case DiagnosticLevel::Error:
      return "error";
  }
  return "unknown";
}

}  // namespace compiler::support
