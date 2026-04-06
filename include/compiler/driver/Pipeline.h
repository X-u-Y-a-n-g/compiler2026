#pragma once

#include "compiler/driver/CompilerInvocation.h"
#include "compiler/support/Diagnostic.h"

namespace compiler::driver {

class Pipeline {
 public:
  int execute(const CompilerInvocation& invocation,
              compiler::support::DiagnosticEngine& diagnostics) const;
};

}  // namespace compiler::driver
