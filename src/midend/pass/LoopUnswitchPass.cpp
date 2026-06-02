#include "../../../include/compiler/midend/pass/LoopUnswitchPass.h"

#include "../../../include/compiler/midend/analysis/LoopAnalysis.h"

namespace compiler::midend::pass {

bool LoopUnswitchPass::run(compiler::ir::core::Module& module) {
  for (const auto& function : module.functions()) {
    (void)compiler::midend::analysis::analyzeNaturalLoops(function);
  }
  return false;
}

}  // namespace compiler::midend::pass
