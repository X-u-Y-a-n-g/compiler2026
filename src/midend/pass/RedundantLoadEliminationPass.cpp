#include "../../../include/compiler/midend/pass/RedundantLoadEliminationPass.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/midend/analysis/FunctionEffectAnalysis.h"
#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::midend::analysis::FunctionEffectAnalysis;

enum class ObjectKind { Unknown, Global, Stack };

struct BaseObject {
  ObjectKind kind = ObjectKind::Unknown;
  std::string identity;

  [[nodiscard]] bool known() const { return kind != ObjectKind::Unknown; }
  bool operator==(const BaseObject& other) const {
    return kind == other.kind && identity == other.identity;
  }
};

struct AvailableValue {
  std::string value;
  BaseObject object;
};

std::string loadKey(const std::vector<std::string>& operands) {
  return operands[0] + "\n" + operands[1];
}

std::string calledFunctionName(const Instruction& instruction) {
  if (instruction.opcode() != "call" || instruction.operands().empty()) {
    return {};
  }
  const auto& operand = instruction.operands().front();
  return operand.size() >= 2 && operand.front() == '@' ? operand.substr(1)
                                                       : std::string{};
}

bool isPointerDerivation(const Instruction& instruction) {
  const auto& opcode = instruction.opcode();
  return opcode == "getelementptr" || opcode == "getelementptr.linear" ||
         opcode == "gep" || opcode == "gep.linear" || opcode == "cast";
}

BaseObject objectForAddress(
    const std::string& address,
    const std::unordered_map<std::string, BaseObject>& derivedObjects) {
  if (!address.empty() && address.front() == '@') {
    return BaseObject{ObjectKind::Global, address};
  }
  const auto found = derivedObjects.find(address);
  return found == derivedObjects.end() ? BaseObject{} : found->second;
}

std::unordered_map<std::string, BaseObject> deriveBaseObjects(
    const Function& function) {
  std::unordered_map<std::string, BaseObject> objects;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult()) {
        objects.emplace(instruction.result(),
                        BaseObject{ObjectKind::Stack,
                                   instruction.result()});
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& block : function.blocks()) {
      for (const auto& instruction : block.instructions()) {
        if (!instruction.hasResult() ||
            objects.find(instruction.result()) != objects.end()) {
          continue;
        }
        std::optional<BaseObject> object;
        if (isPointerDerivation(instruction) &&
            !instruction.operands().empty()) {
          const auto candidate =
              objectForAddress(instruction.operands().front(), objects);
          if (candidate.known()) object = candidate;
        } else if (instruction.opcode() == "select" &&
                   instruction.operands().size() == 3) {
          const auto lhs = objectForAddress(instruction.operands()[1], objects);
          const auto rhs = objectForAddress(instruction.operands()[2], objects);
          if (lhs.known() && lhs == rhs) object = lhs;
        } else if (instruction.opcode() == "phi" &&
                   instruction.operands().size() >= 3) {
          bool consistent = true;
          bool sawValue = false;
          BaseObject common;
          for (std::size_t index = 1;
               index + 1 < instruction.operands().size(); index += 2) {
            const auto candidate =
                objectForAddress(instruction.operands()[index], objects);
            if (!candidate.known()) {
              consistent = false;
              break;
            }
            if (!sawValue) {
              common = candidate;
              sawValue = true;
            } else if (!(candidate == common)) {
              consistent = false;
              break;
            }
          }
          if (consistent && sawValue) object = common;
        }
        if (object.has_value()) {
          objects.emplace(instruction.result(), *object);
          changed = true;
        }
      }
    }
  }
  return objects;
}

std::unordered_set<std::string> collectNonEscapedScalarSlots(const Function& function) {
  std::unordered_set<std::string> slots;
  std::unordered_set<std::string> escaped;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == "alloca" && instruction.hasResult() &&
          instruction.operands().size() == 1 &&
          instruction.operands()[0].find('[') == std::string::npos) {
        slots.insert(instruction.result());
      }
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (std::size_t index = 0; index < instruction.operands().size(); ++index) {
        const auto& operand = instruction.operands()[index];
        if (slots.find(operand) == slots.end()) {
          continue;
        }
        const bool isLoadAddress = instruction.opcode() == "load" && index == 1;
        const bool isStoreAddress = instruction.opcode() == "store" && index == 2;
        if (!isLoadAddress && !isStoreAddress) {
          escaped.insert(operand);
        }
      }
    }
  }
  for (const auto& slot : escaped) {
    slots.erase(slot);
  }
  return slots;
}

void rewriteAliases(Function& function,
                    const std::unordered_map<std::string, std::string>& aliases) {
  if (aliases.empty()) {
    return;
  }
  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      rewritten.emplace_back(instruction.opcode(),
                             detail::rewriteOperands(instruction.operands(), aliases),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
}

void eliminateRedundantLoadsLegacy(Function& function) {
  std::unordered_map<std::string, std::string> aliases;
  const auto localSlots = collectNonEscapedScalarSlots(function);

  for (auto& block : function.blocks()) {
    std::unordered_map<std::string, std::string> availableLoads;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());

    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          operands.size() == 2) {
        const auto key = loadKey(operands);
        const auto available = availableLoads.find(key);
        if (available != availableLoads.end()) {
          aliases[instruction.result()] = detail::resolveAlias(available->second, aliases);
          continue;
        }
        availableLoads.emplace(key, instruction.result());
      }

      if (instruction.opcode() == "store" && operands.size() == 3 &&
          localSlots.find(operands[2]) != localSlots.end()) {
        availableLoads[operands[0] + "\n" + operands[2]] = operands[1];
      } else if (instruction.opcode() == "store" || instruction.opcode() == "call") {
        for (auto available = availableLoads.begin(); available != availableLoads.end();) {
          const auto separator = available->first.find('\n');
          const std::string address =
              separator == std::string::npos ? std::string{} : available->first.substr(separator + 1);
          if (localSlots.find(address) == localSlots.end()) {
            available = availableLoads.erase(available);
          } else {
            ++available;
          }
        }
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  rewriteAliases(function, aliases);
}

bool isIsolatedStack(
    const BaseObject& object,
    const std::unordered_set<std::string>& nonEscapedScalarSlots) {
  return object.kind == ObjectKind::Stack &&
         nonEscapedScalarSlots.find(object.identity) !=
             nonEscapedScalarSlots.end();
}

void invalidateForStore(
    std::unordered_map<std::string, AvailableValue>& available,
    const BaseObject& storedObject,
    const std::unordered_set<std::string>& nonEscapedScalarSlots) {
  for (auto iterator = available.begin(); iterator != available.end();) {
    const auto& loadedObject = iterator->second.object;
    bool invalidated = false;
    if (!storedObject.known()) {
      invalidated = !isIsolatedStack(loadedObject, nonEscapedScalarSlots);
    } else {
      invalidated = loadedObject == storedObject ||
                    (!loadedObject.known() &&
                     !isIsolatedStack(storedObject,
                                      nonEscapedScalarSlots));
    }
    if (invalidated) {
      iterator = available.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

void invalidateForCall(
    std::unordered_map<std::string, AvailableValue>& available,
    const Instruction& call, const FunctionEffectAnalysis& functionEffects,
    const std::unordered_set<std::string>& nonEscapedScalarSlots) {
  const auto& effects = functionEffects.effectFor(calledFunctionName(call));
  if (!effects.writesExternalMemory && !effects.hasOtherSideEffects) return;
  for (auto iterator = available.begin(); iterator != available.end();) {
    if (isIsolatedStack(iterator->second.object,
                        nonEscapedScalarSlots)) {
      ++iterator;
    } else {
      iterator = available.erase(iterator);
    }
  }
}

void eliminateObjectSensitiveLoads(
    Function& function, const FunctionEffectAnalysis& functionEffects) {
  std::unordered_map<std::string, std::string> aliases;
  const auto nonEscapedScalarSlots =
      collectNonEscapedScalarSlots(function);
  const auto objects = deriveBaseObjects(function);

  for (auto& block : function.blocks()) {
    std::unordered_map<std::string, AvailableValue> available;
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      auto operands = detail::rewriteOperands(instruction.operands(), aliases);
      if (instruction.opcode() == "load" && instruction.hasResult() &&
          operands.size() == 2) {
        const auto key = loadKey(operands);
        const auto found = available.find(key);
        if (found != available.end()) {
          aliases[instruction.result()] =
              detail::resolveAlias(found->second.value, aliases);
          continue;
        }
        available.emplace(
            key, AvailableValue{instruction.result(),
                                objectForAddress(operands[1], objects)});
      } else if (instruction.opcode() == "store" && operands.size() == 3) {
        const auto object = objectForAddress(operands[2], objects);
        invalidateForStore(available, object, nonEscapedScalarSlots);
        available[operands[0] + "\n" + operands[2]] =
            AvailableValue{detail::resolveAlias(operands[1], aliases), object};
      } else if (instruction.opcode() == "call") {
        const Instruction rewrittenCall{instruction.opcode(), operands,
                                        instruction.result()};
        invalidateForCall(available, rewrittenCall, functionEffects,
                          nonEscapedScalarSlots);
      }
      rewritten.emplace_back(instruction.opcode(), std::move(operands),
                             instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }
  rewriteAliases(function, aliases);
}

}  // namespace

void RedundantLoadEliminationPass::run(compiler::ir::core::Module& module) {
  const bool legacy =
      std::getenv("COMPILER_DISABLE_OBJECT_SENSITIVE_LOAD_FORWARDING") !=
      nullptr;
  const FunctionEffectAnalysis functionEffects(module);
  for (auto& function : module.functions()) {
    if (legacy) {
      eliminateRedundantLoadsLegacy(function);
    } else {
      eliminateObjectSensitiveLoads(function, functionEffects);
    }
  }
}

}  // namespace compiler::midend::pass
