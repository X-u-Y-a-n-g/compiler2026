#include "../../../include/compiler/midend/pass/GlobalDCEPass.h"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Module;

std::unordered_set<std::string> referencedGlobals(const Module& module) {
  std::unordered_set<std::string> referenced;
  for (const auto& function : module.functions()) {
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        for (const auto& operand : instruction.operands()) {
          if (!operand.empty() && operand.front() == '@') {
            referenced.insert(operand.substr(1));
          }
        }
      }
    }
  }
  return referenced;
}

bool runGlobalDCE(Module& module) {
  const auto referenced = referencedGlobals(module);
  const auto oldSize = module.globals().size();
  module.globals().erase(
      std::remove_if(module.globals().begin(), module.globals().end(),
                     [&](const GlobalVariable& global) {
                       return referenced.find(global.name()) == referenced.end();
                     }),
      module.globals().end());
  return oldSize != module.globals().size();
}

}  // namespace

bool GlobalDCEPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  runGlobalDCE(module);
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
