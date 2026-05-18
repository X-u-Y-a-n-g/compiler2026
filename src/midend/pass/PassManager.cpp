#include "PassManager.h"

namespace compiler::midend::pass {

void PassManager::add(std::unique_ptr<ModulePass> pass) {
  passes_.push_back(std::move(pass));
}

void PassManager::run(compiler::ir::core::Module& module) const {
  for (const auto& pass : passes_) {
    module.addPassNote(pass->name());
    pass->run(module);
  }
}

}  // namespace compiler::midend::pass
