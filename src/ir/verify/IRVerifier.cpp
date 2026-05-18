#include "IRVerifier.h"

#include <unordered_set>

namespace compiler::ir::verify {

namespace {

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

std::vector<std::string> IRVerifier::verify(const compiler::ir::core::Module& module) const {
  std::vector<std::string> errors;

  std::unordered_set<std::string> globalNames;
  for (const auto& global : module.globals()) {
    if (global.name().empty()) {
      errors.push_back("global variable name must not be empty");
      continue;
    }
    if (!globalNames.insert(global.name()).second) {
      errors.push_back("duplicate global variable @" + global.name());
    }
    if (global.type().empty()) {
      errors.push_back("global variable @" + global.name() + " must have a type");
    }
  }

  std::unordered_set<std::string> functionNames;
  for (const auto& function : module.functions()) {
    if (function.name().empty()) {
      errors.push_back("function name must not be empty");
      continue;
    }
    if (!functionNames.insert(function.name()).second) {
      errors.push_back("duplicate function @" + function.name());
    }
    if (function.returnType().empty()) {
      errors.push_back("function @" + function.name() + " must have a return type");
    }
    if (function.blocks().empty()) {
      errors.push_back("function @" + function.name() + " must contain at least one basic block");
      continue;
    }

    std::unordered_set<std::string> blockNames;
    std::unordered_set<std::string> valueNames;
    for (const auto& block : function.blocks()) {
      if (block.name().empty()) {
        errors.push_back("function @" + function.name() + " contains a block with empty name");
        continue;
      }
      if (!blockNames.insert(block.name()).second) {
        errors.push_back("function @" + function.name() + " has duplicate block '" + block.name() +
                         "'");
      }
      if (block.empty()) {
        errors.push_back("block '" + block.name() + "' in function @" + function.name() +
                         " must not be empty");
        continue;
      }

      bool sawTerminator = false;
      for (const auto& instruction : block.instructions()) {
        if (instruction.hasResult()) {
          if (!startsWith(instruction.result(), "%")) {
            errors.push_back("instruction result '" + instruction.result() + "' in block '" +
                             block.name() + "' must start with '%'");
          }
          if (!valueNames.insert(instruction.result()).second) {
            errors.push_back("duplicate SSA-style value name '" + instruction.result() +
                             "' in function @" + function.name());
          }
        }

        if (instruction.isTerminator()) {
          if (sawTerminator) {
            errors.push_back("block '" + block.name() + "' in function @" + function.name() +
                             " contains multiple terminators");
          }
          sawTerminator = true;
        } else if (sawTerminator) {
          errors.push_back("block '" + block.name() + "' in function @" + function.name() +
                           " has instructions after a terminator");
        }

        if (instruction.opcode() == "br") {
          if (instruction.operands().size() != 1 && instruction.operands().size() != 3) {
            errors.push_back("br in block '" + block.name() +
                             "' must have either one target or condition, true target and false target");
          }
        } else if (instruction.opcode() == "ret") {
          const bool isVoid = function.returnType() == "void";
          if (isVoid && !instruction.operands().empty()) {
            errors.push_back("void function @" + function.name() +
                             " must not return a value in block '" + block.name() + "'");
          }
          if (!isVoid && instruction.operands().size() != 1) {
            errors.push_back("non-void function @" + function.name() +
                             " must return exactly one value in block '" + block.name() + "'");
          }
        }
      }

      if (!block.isTerminated()) {
        errors.push_back("block '" + block.name() + "' in function @" + function.name() +
                         " must end with a terminator");
      }
    }

    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (instruction.opcode() == "br" && instruction.operands().size() == 1 &&
            blockNames.find(instruction.operands().front()) == blockNames.end()) {
          errors.push_back("br in block '" + block.name() + "' targets unknown block '" +
                           instruction.operands().front() + "'");
        }
        if (instruction.opcode() == "br" && instruction.operands().size() == 3) {
          if (blockNames.find(instruction.operands()[1]) == blockNames.end()) {
            errors.push_back("br in block '" + block.name() + "' targets unknown block '" +
                             instruction.operands()[1] + "'");
          }
          if (blockNames.find(instruction.operands()[2]) == blockNames.end()) {
            errors.push_back("br in block '" + block.name() + "' targets unknown block '" +
                             instruction.operands()[2] + "'");
          }
        }
      }
    }
  }

  return errors;
}

}  // namespace compiler::ir::verify
