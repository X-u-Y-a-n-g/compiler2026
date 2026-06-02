#pragma once

#include "PassManager.h"

namespace compiler::midend::pass {

class PipelineBuilder {
public:
  static PassManager buildO0Pipeline();
  static PassManager buildO1Pipeline();
  static PassManager buildBasicCleanPipeline();
  static PassManager buildSsaEnablingPipeline();
  static PassManager buildInlinePipeline();
  static PassManager buildLateInlinePipeline();
  static PassManager buildLoopPipeline();
};

} // namespace compiler::midend::pass
