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
  static PassManager buildSccpCleanPipeline();
  static PassManager buildScalarPipeline();
  static PassManager buildMemoryPipeline();
  static PassManager buildLoopPipeline();
  static PassManager buildModulePipeline();
  static PassManager buildFinalCleanupPipeline();
};

} // namespace compiler::midend::pass
