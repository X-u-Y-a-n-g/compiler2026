#include "../../../include/compiler/midend/pass/BasicAliasAnalysisPass.h"

namespace compiler::midend::pass {

bool BasicAliasAnalysisPass::run(compiler::ir::core::Module& module) {
  (void)module;
  return false;
}

}  // namespace compiler::midend::pass
