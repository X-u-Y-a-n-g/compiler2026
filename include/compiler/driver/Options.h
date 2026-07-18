#pragma once

#include <string>

namespace compiler::driver {

struct Options {
  std::string inputPath;
  std::string outputPath;
  std::string target = "aarch64";
  bool emitAssembly = true;
  bool dumpTokens = false;
  bool dumpAst = false;
  bool dumpIr = false;
  int optLevel = 0;
};

}  // namespace compiler::driver
