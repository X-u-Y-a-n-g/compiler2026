#pragma once

#include "CompilerInvocation.h"
#include "Diagnostic.h"

namespace compiler::driver {

class Pipeline {
 public:
  int execute(const CompilerInvocation& invocation,
              compiler::support::DiagnosticEngine& diagnostics) const;
};

}  // namespace compiler::driver
