#include "../../../include/compiler/midend/pass/PassManager.h"

#include "../../../include/compiler/ir/verify/IRVerifier.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace compiler::midend::pass {

namespace {

bool envEnabled(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  const std::string text = value;
  return !text.empty() && text != "0" && text != "false" && text != "FALSE" &&
         text != "off" && text != "OFF";
}

std::unordered_set<std::string> envNameSet(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return {};
  }

  std::unordered_set<std::string> names;
  std::string current;
  for (const char ch : std::string(value)) {
    if (ch == ',' || ch == ';' || ch == ' ') {
      if (!current.empty()) {
        names.insert(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    names.insert(current);
  }
  return names;
}

bool passEnabled(const std::string &passName,
                 const std::unordered_set<std::string> &onlyPasses,
                 const std::unordered_set<std::string> &disabledPasses) {
  if (!onlyPasses.empty() && onlyPasses.find(passName) == onlyPasses.end()) {
    return false;
  }
  return disabledPasses.find(passName) == disabledPasses.end();
}

void verifyAfterPass(const compiler::ir::core::Module &module,
                     const std::string &passName) {
  const auto errors = compiler::ir::verify::IRVerifier{}.verify(module);
  if (errors.empty()) {
    return;
  }

  std::ostringstream message;
  message << "IR verifier failed after pass '" << passName << "'";
  for (const auto &error : errors) {
    message << "\n  " << error;
  }
  throw std::runtime_error(message.str());
}

} // namespace

void PassManager::add(std::unique_ptr<ModulePass> pass) {
  passes_.push_back(std::move(pass));
}

bool PassManager::run(compiler::ir::core::Module &module) const {
  bool changed = false;
  const bool debugPasses = envEnabled("COMPILER_DEBUG_PASSES");
  const bool verifyEachPass = envEnabled("COMPILER_VERIFY_EACH_PASS");
  const auto onlyPasses = envNameSet("COMPILER_ONLY_PASSES");
  const auto disabledPasses = envNameSet("COMPILER_DISABLE_PASSES");
  for (const auto &pass : passes_) {
    const auto passName = pass->name();
    if (!passEnabled(passName, onlyPasses, disabledPasses)) {
      if (debugPasses) {
        std::cerr << "[pass] " << passName << " skipped\n";
      }
      continue;
    }

    module.addPassNote(passName);
    const bool passChanged = pass->run(module);
    changed |= passChanged;
    if (debugPasses) {
      std::cerr << "[pass] " << passName
                << " changed=" << (passChanged ? "yes" : "no") << '\n';
    }
    if (verifyEachPass) {
      verifyAfterPass(module, passName);
    }
  }
  return changed;
}

bool PassManager::runFixedPoint(compiler::ir::core::Module &module,
                                int maxRounds) const {
  bool changedAny = false;
  const bool debugPasses = envEnabled("COMPILER_DEBUG_PASSES");
  for (int round = 0; round < maxRounds; ++round) {
    const bool changed = run(module);
    changedAny |= changed;
    if (debugPasses) {
      std::cerr << "[pass-fixed-point] round=" << (round + 1)
                << " changed=" << (changed ? "yes" : "no") << '\n';
    }
    if (!changed) {
      break;
    }
  }
  return changedAny;
}

} // namespace compiler::midend::pass
