#include "FileSystem.h"

#include <fstream>
#include <sstream>

namespace compiler::support {

bool FileSystem::readTextFile(const std::string& path, std::string& out,
                              DiagnosticEngine& diagnostics) {
  std::ifstream input(path);
  if (!input) {
    diagnostics.error({}, "failed to open input file: " + path);
    return false;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  out = buffer.str();
  return true;
}

bool FileSystem::writeTextFile(const std::string& path, const std::string& content,
                               DiagnosticEngine& diagnostics) {
  std::ofstream output(path);
  if (!output) {
    diagnostics.error({}, "failed to open output file: " + path);
    return false;
  }

  output << content;
  return true;
}

}  // namespace compiler::support
