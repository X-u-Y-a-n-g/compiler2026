#include "../../../include/compiler/midend/pass/InternalizePass.h"

namespace compiler::midend::pass {

bool InternalizePass::run(compiler::ir::core::Module& module) {
  (void)module;
  return false;
}

}  // namespace compiler::midend::pass
