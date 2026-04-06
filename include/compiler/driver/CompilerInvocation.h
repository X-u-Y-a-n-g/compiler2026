#pragma once

#include <vector>

#include "compiler/driver/Options.h"
#include "compiler/support/Diagnostic.h"

namespace compiler::driver {

class CompilerInvocation {
 public:
  static CompilerInvocation fromCommandLine(int argc, const char* const* argv,
                                            compiler::support::DiagnosticEngine& diagnostics);

  [[nodiscard]] const Options& options() const;
  [[nodiscard]] bool valid() const;

 private:
  Options options_;
  bool valid_ = false;
};

}  // namespace compiler::driver
