#pragma once

#include "PassManager.h"

namespace compiler::midend::pass {

class PipelineBuilder {
 public:
  static PassManager buildO0Pipeline();
  static PassManager buildO1Pipeline();
  static PassManager buildBasicCleanPipeline();
  static PassManager buildInlinePipeline();
  static PassManager buildLoopPipeline();
};

}  // namespace compiler::midend::pass
