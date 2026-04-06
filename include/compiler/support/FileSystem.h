#pragma once

#include <string>

#include "compiler/support/Diagnostic.h"

namespace compiler::support {

class FileSystem {
 public:
  static bool readTextFile(const std::string& path, std::string& out,
                           DiagnosticEngine& diagnostics);
  static bool writeTextFile(const std::string& path, const std::string& content,
                            DiagnosticEngine& diagnostics);
};

}  // namespace compiler::support
