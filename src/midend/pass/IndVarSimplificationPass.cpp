#include "../../../include/compiler/midend/pass/IndVarSimplificationPass.h"

#include "../../../include/compiler/midend/analysis/LoopAnalysis.h"

namespace compiler::midend::pass {

bool IndVarSimplificationPass::run(compiler::ir::core::Module& module) {
  for (const auto& function : module.functions()) {
    (void)compiler::midend::analysis::analyzeNaturalLoops(function);
  }
  return false;
}

}  // namespace compiler::midend::pass
