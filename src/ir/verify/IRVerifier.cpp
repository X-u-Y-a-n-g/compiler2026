#include "../../../include/compiler/ir/verify/IRVerifier.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compiler::ir::verify {

namespace {

bool startsWith(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

bool isValueReference(const std::string &operand) {
  return !operand.empty() && operand.front() == '%';
}

std::vector<std::string>
branchTargets(const compiler::ir::core::Instruction &instruction) {
  if (instruction.opcode() != "br") {
    return {};
  }
  const auto &operands = instruction.operands();
  if (operands.size() == 1) {
    return {operands[0]};
  }
  if (operands.size() == 3) {
    if (operands[1] == operands[2]) {
      return {operands[1]};
    }
    return {operands[1], operands[2]};
  }
  return {};
}

std::vector<std::string>
usedLocalValues(const compiler::ir::core::Instruction &instruction) {
  std::vector<std::string> uses;
  const auto &operands = instruction.operands();

  if (instruction.opcode() == "phi") {
    for (std::size_t i = 1; i + 1 < operands.size(); i += 2) {
      if (isValueReference(operands[i])) {
        uses.push_back(operands[i]);
      }
    }
    return uses;
  }

  if (instruction.opcode() == "br") {
    if (operands.size() == 3 && isValueReference(operands[0])) {
      uses.push_back(operands[0]);
    }
    return uses;
  }

  if (instruction.opcode() == "ret") {
    for (const auto &operand : operands) {
      if (isValueReference(operand)) {
        uses.push_back(operand);
      }
    }
    return uses;
  }

  for (const auto &operand : operands) {
    if (isValueReference(operand)) {
      uses.push_back(operand);
    }
  }
  return uses;
}

} // namespace

std::vector<std::string>
IRVerifier::verify(const compiler::ir::core::Module &module) const {
  std::vector<std::string> errors;

  std::unordered_set<std::string> globalNames;
  for (const auto &global : module.globals()) {
    if (global.name().empty()) {
      errors.push_back("global variable name must not be empty");
      continue;
    }
    if (!globalNames.insert(global.name()).second) {
      errors.push_back("duplicate global variable @" + global.name());
    }
    if (global.type().empty()) {
      errors.push_back("global variable @" + global.name() +
                       " must have a type");
    }
  }

  std::unordered_set<std::string> functionNames;
  for (const auto &function : module.functions()) {
    if (function.name().empty()) {
      errors.push_back("function name must not be empty");
      continue;
    }
    if (!functionNames.insert(function.name()).second) {
      errors.push_back("duplicate function @" + function.name());
    }
    if (function.returnType().empty()) {
      errors.push_back("function @" + function.name() +
                       " must have a return type");
    }
    if (function.blocks().empty()) {
      errors.push_back("function @" + function.name() +
                       " must contain at least one basic block");
      continue;
    }

    std::unordered_set<std::string> blockNames;
    std::unordered_set<std::string> valueNames;
    for (const auto &parameter : function.parameters()) {
      if (parameter.name.empty()) {
        errors.push_back("function @" + function.name() +
                         " contains a parameter with empty name");
        continue;
      }
      if (!valueNames.insert(parameter.name).second) {
        errors.push_back("duplicate SSA-style value name '" + parameter.name +
                         "' in function @" + function.name());
      }
    }
    for (const auto &block : function.blocks()) {
      if (block.name().empty()) {
        errors.push_back("function @" + function.name() +
                         " contains a block with empty name");
        continue;
      }
      if (!blockNames.insert(block.name()).second) {
        errors.push_back("function @" + function.name() +
                         " has duplicate block '" + block.name() + "'");
      }
      if (block.empty()) {
        errors.push_back("block '" + block.name() + "' in function @" +
                         function.name() + " must not be empty");
        continue;
      }

      bool sawTerminator = false;
      for (const auto &instruction : block.instructions()) {
        if (instruction.hasResult()) {
          if (!startsWith(instruction.result(), "%")) {
            errors.push_back("instruction result '" + instruction.result() +
                             "' in block '" + block.name() +
                             "' must start with '%'");
          }
          if (!valueNames.insert(instruction.result()).second) {
            errors.push_back("duplicate SSA-style value name '" +
                             instruction.result() + "' in function @" +
                             function.name());
          }
        }

        if (instruction.isTerminator()) {
          if (sawTerminator) {
            errors.push_back("block '" + block.name() + "' in function @" +
                             function.name() +
                             " contains multiple terminators");
          }
          sawTerminator = true;
        } else if (sawTerminator) {
          errors.push_back("block '" + block.name() + "' in function @" +
                           function.name() +
                           " has instructions after a terminator");
        }

        if (instruction.opcode() == "br") {
          if (instruction.operands().size() != 1 &&
              instruction.operands().size() != 3) {
            errors.push_back("br in block '" + block.name() +
                             "' must have either one target or condition, true "
                             "target and false target");
          }
        } else if (instruction.opcode() == "ret") {
          const bool isVoid = function.returnType() == "void";
          if (isVoid && !instruction.operands().empty()) {
            errors.push_back("void function @" + function.name() +
                             " must not return a value in block '" +
                             block.name() + "'");
          }
          if (!isVoid && instruction.operands().size() != 1) {
            errors.push_back("non-void function @" + function.name() +
                             " must return exactly one value in block '" +
                             block.name() + "'");
          }
        }

        if (instruction.opcode() != "phi" && instruction.hasResult()) {
          for (const auto &operand : instruction.operands()) {
            if (isValueReference(operand) && operand == instruction.result()) {
              errors.push_back("instruction result '" + instruction.result() +
                               "' in block '" + block.name() +
                               "' in function @" + function.name() +
                               " is used by its own definition");
            }
          }
        }
      }

      if (!block.isTerminated()) {
        errors.push_back("block '" + block.name() + "' in function @" +
                         function.name() + " must end with a terminator");
      }
    }

    std::unordered_map<std::string, std::unordered_set<std::string>>
        predecessors;
    for (const auto &block : function.blocks()) {
      predecessors.emplace(block.name(), std::unordered_set<std::string>{});
    }
    for (const auto &block : function.blocks()) {
      if (const auto *terminator = block.terminator()) {
        for (const auto &target : branchTargets(*terminator)) {
          predecessors[target].insert(block.name());
        }
      }
    }

    for (const auto &block : function.blocks()) {
      bool sawNonPhi = false;
      for (const auto &instruction : block.instructions()) {
        if (instruction.opcode() == "phi") {
          if (sawNonPhi) {
            errors.push_back("phi in block '" + block.name() +
                             "' in function @" + function.name() +
                             " appears after a non-phi instruction");
          }
          const auto &operands = instruction.operands();
          if (operands.empty() || operands.size() % 2 == 0) {
            errors.push_back(
                "phi in block '" + block.name() + "' in function @" +
                function.name() +
                " must have a type followed by value/block incoming pairs");
          }

          std::unordered_set<std::string> incomingBlocks;
          for (std::size_t i = 2; i < operands.size(); i += 2) {
            const auto &incomingBlock = operands[i];
            if (blockNames.find(incomingBlock) == blockNames.end()) {
              errors.push_back("phi in block '" + block.name() +
                               "' in function @" + function.name() +
                               " references unknown predecessor block '" +
                               incomingBlock + "'");
              continue;
            }
            if (!incomingBlocks.insert(incomingBlock).second) {
              errors.push_back("phi in block '" + block.name() +
                               "' in function @" + function.name() +
                               " has duplicate incoming block '" +
                               incomingBlock + "'");
            }
          }

          const auto predIt = predecessors.find(block.name());
          const std::unordered_set<std::string> expectedPreds =
              predIt == predecessors.end() ? std::unordered_set<std::string>{}
                                           : predIt->second;
          for (const auto &pred : expectedPreds) {
            if (incomingBlocks.find(pred) == incomingBlocks.end()) {
              errors.push_back("phi in block '" + block.name() +
                               "' in function @" + function.name() +
                               " is missing incoming value from predecessor '" +
                               pred + "'");
            }
          }
          for (const auto &incomingBlock : incomingBlocks) {
            if (expectedPreds.find(incomingBlock) == expectedPreds.end()) {
              errors.push_back(
                  "phi in block '" + block.name() + "' in function @" +
                  function.name() +
                  " has incoming value from non-predecessor block '" +
                  incomingBlock + "'");
            }
          }
        } else {
          sawNonPhi = true;
        }

        if (instruction.opcode() == "br" &&
            instruction.operands().size() == 1 &&
            blockNames.find(instruction.operands().front()) ==
                blockNames.end()) {
          errors.push_back("br in block '" + block.name() +
                           "' targets unknown block '" +
                           instruction.operands().front() + "'");
        }
        if (instruction.opcode() == "br" &&
            instruction.operands().size() == 3) {
          if (blockNames.find(instruction.operands()[1]) == blockNames.end()) {
            errors.push_back("br in block '" + block.name() +
                             "' targets unknown block '" +
                             instruction.operands()[1] + "'");
          }
          if (blockNames.find(instruction.operands()[2]) == blockNames.end()) {
            errors.push_back("br in block '" + block.name() +
                             "' targets unknown block '" +
                             instruction.operands()[2] + "'");
          }
        }
      }
    }

    for (const auto &block : function.blocks()) {
      for (const auto &instruction : block.instructions()) {
        for (const auto &use : usedLocalValues(instruction)) {
          if (valueNames.find(use) == valueNames.end()) {
            errors.push_back("instruction in block '" + block.name() +
                             "' in function @" + function.name() +
                             " references undefined local value '" + use + "'");
          }
        }
      }
    }
  }

  return errors;
}

} // namespace compiler::ir::verify
