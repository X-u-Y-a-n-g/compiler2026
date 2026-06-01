#include "../../../include/compiler/midend/pass/PassManager.h"

namespace compiler::midend::pass {

void PassManager::add(std::unique_ptr<ModulePass> pass) {
  passes_.push_back(std::move(pass));
}

bool PassManager::run(compiler::ir::core::Module& module) const {
  bool changed = false;
  for (const auto& pass : passes_) {
    module.addPassNote(pass->name());
    changed |= pass->run(module);
  }
  return changed;
}

bool PassManager::runFixedPoint(compiler::ir::core::Module& module, int maxRounds) const {
  bool changedAny = false;
  for (int round = 0; round < maxRounds; ++round) {
    const bool changed = run(module);
    changedAny |= changed;
    if (!changed) {
      break;
    }
  }
  return changedAny;
}

}  // namespace compiler::midend::pass
