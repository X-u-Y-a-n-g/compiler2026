#include "../../../include/compiler/midend/pass/LoopSimplificationPass.h"

#include "../../../include/compiler/midend/analysis/LoopAnalysis.h"

namespace compiler::midend::pass {

bool LoopSimplificationPass::run(compiler::ir::core::Module& module) {
  for (const auto& function : module.functions()) {
    (void)compiler::midend::analysis::analyzeNaturalLoops(function);
  }
  return false;
}

}  // namespace compiler::midend::pass
