#include "../../../include/compiler/midend/pass/ArrayAccessSimplificationPass.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct ArrayType {
  std::string elementType;
  std::vector<std::optional<long long>> dimensions;
};

struct AccessPath {
  std::string root;
  ArrayType type;
  std::vector<std::string> indexes;
};

bool isGepOpcode(const std::string& opcode) {
  return opcode == "getelementptr" || opcode == "gep";
}

bool parseArrayType(const std::string& text, ArrayType& type) {
  const auto firstBracket = text.find('[');
  type.elementType = firstBracket == std::string::npos ? text : text.substr(0, firstBracket);
  type.dimensions.clear();

  std::size_t index = firstBracket;
  while (index != std::string::npos && index < text.size()) {
    const auto close = text.find(']', index);
    if (close == std::string::npos) {
      return false;
    }
    if (close == index + 1) {
      type.dimensions.push_back(std::nullopt);
    } else {
      long long extent = 0;
      if (!detail::parseIntegerLiteral(text.substr(index + 1, close - index - 1), extent) ||
          extent <= 0) {
        return false;
      }
      type.dimensions.push_back(extent);
    }
    index = text.find('[', close + 1);
  }
  return !type.elementType.empty();
}

std::unordered_map<std::string, ArrayType> collectRootTypes(const Function& function) {
  std::unordered_map<std::string, ArrayType> roots;
  for (const auto& parameter : function.parameters()) {
    ArrayType type;
    if (parseArrayType(parameter.type, type) && !type.dimensions.empty()) {
      roots.emplace(parameter.name, std::move(type));
    }
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() != "alloca" || !instruction.hasResult() ||
          instruction.operands().size() != 1) {
        continue;
      }
      ArrayType type;
      if (parseArrayType(instruction.operands().front(), type) && !type.dimensions.empty()) {
        roots.emplace(instruction.result(), std::move(type));
      }
    }
  }
  return roots;
}

void seedUsedNames(const Function& function, std::unordered_set<std::string>& used) {
  for (const auto& parameter : function.parameters()) {
    used.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) {
        used.insert(instruction.result());
      }
    }
  }
}

std::string freshName(const Function& function, const std::string& prefix,
                      std::unordered_set<std::string>& used) {
  if (used.empty()) {
    seedUsedNames(function, used);
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = "%" + prefix + std::to_string(index);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::optional<long long> strideFor(const ArrayType& type, std::size_t index) {
  if (index >= type.dimensions.size()) {
    return std::nullopt;
  }
  long long stride = 1;
  for (std::size_t i = index + 1; i < type.dimensions.size(); ++i) {
    if (!type.dimensions[i].has_value()) {
      return std::nullopt;
    }
    stride *= *type.dimensions[i];
  }
  return stride;
}

std::string makeScaledIndex(const Function& function, const std::string& index, long long stride,
                            std::vector<Instruction>& prefix,
                            std::unordered_set<std::string>& used) {
  if (stride == 1) {
    return index;
  }

  long long constant = 0;
  if (detail::parseIntegerLiteral(index, constant)) {
    return detail::formatIntegerLiteral(constant * stride);
  }

  const std::string scaled = freshName(function, "aas.mul", used);
  prefix.emplace_back("mul", std::vector<std::string>{index, detail::formatIntegerLiteral(stride)},
                      scaled);
  return scaled;
}

std::optional<std::string> buildLinearIndex(const Function& function, const AccessPath& path,
                                            std::vector<Instruction>& prefix,
                                            std::unordered_set<std::string>& used) {
  if (path.indexes.empty() || path.indexes.size() > path.type.dimensions.size()) {
    return std::nullopt;
  }

  long long constantSum = 0;
  std::string valueSum;
  for (std::size_t i = 0; i < path.indexes.size(); ++i) {
    const auto stride = strideFor(path.type, i);
    if (!stride.has_value()) {
      return std::nullopt;
    }

    const auto term = makeScaledIndex(function, path.indexes[i], *stride, prefix, used);
    long long constant = 0;
    if (detail::parseIntegerLiteral(term, constant)) {
      constantSum += constant;
      continue;
    }

    if (valueSum.empty()) {
      if (constantSum != 0) {
        valueSum = detail::formatIntegerLiteral(constantSum);
        constantSum = 0;
        const std::string sum = freshName(function, "aas.add", used);
        prefix.emplace_back("add", std::vector<std::string>{valueSum, term}, sum);
        valueSum = sum;
      } else {
        valueSum = term;
      }
      continue;
    }

    const std::string sum = freshName(function, "aas.add", used);
    prefix.emplace_back("add", std::vector<std::string>{valueSum, term}, sum);
    valueSum = sum;
  }

  if (valueSum.empty()) {
    return detail::formatIntegerLiteral(constantSum);
  }
  if (constantSum == 0) {
    return valueSum;
  }
  const std::string sum = freshName(function, "aas.add", used);
  prefix.emplace_back("add", std::vector<std::string>{valueSum,
                                                      detail::formatIntegerLiteral(constantSum)},
                      sum);
  return sum;
}

std::unordered_map<std::string, std::size_t> computeUseCounts(const Function& function) {
  std::unordered_map<std::string, std::size_t> counts;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& operand : instruction.operands()) {
        if (!operand.empty() && operand.front() == '%') {
          ++counts[operand];
        }
      }
    }
  }
  return counts;
}

bool runArrayAccessSimplify(Function& function) {
  const auto rootTypes = collectRootTypes(function);
  if (rootTypes.empty()) {
    return false;
  }

  const auto useCounts = computeUseCounts(function);
  std::unordered_map<std::string, AccessPath> paths;
  std::unordered_set<std::string> used;
  bool changed = false;

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
      if (!isGepOpcode(instruction.opcode()) || !instruction.hasResult() ||
          instruction.operands().size() != 2) {
        rewritten.push_back(instruction);
        continue;
      }

      const auto& base = instruction.operands()[0];
      const auto& index = instruction.operands()[1];
      std::optional<AccessPath> path;
      if (const auto root = rootTypes.find(base); root != rootTypes.end()) {
        path = AccessPath{base, root->second, {index}};
      } else if (const auto previous = paths.find(base); previous != paths.end()) {
        path = previous->second;
        path->indexes.push_back(index);
      }

      if (!path.has_value()) {
        rewritten.push_back(instruction);
        continue;
      }
      paths[instruction.result()] = *path;

      const bool reachesScalarElement = path->indexes.size() == path->type.dimensions.size();
      const auto useCount = useCounts.find(instruction.result());
      const bool isOnlyUsedAsFinalAddress = useCount == useCounts.end() || useCount->second <= 1;
      if (!reachesScalarElement || !isOnlyUsedAsFinalAddress || path->indexes.size() < 2) {
        rewritten.push_back(instruction);
        continue;
      }

      std::vector<Instruction> prefix;
      const auto linearIndex = buildLinearIndex(function, *path, prefix, used);
      if (!linearIndex.has_value()) {
        rewritten.push_back(instruction);
        continue;
      }

      rewritten.insert(rewritten.end(), prefix.begin(), prefix.end());
      rewritten.emplace_back("getelementptr.linear",
                             std::vector<std::string>{path->root, *linearIndex},
                             instruction.result());
      changed = true;
    }
    block.instructions() = std::move(rewritten);
  }

  return changed;
}

}  // namespace

bool ArrayAccessSimplificationPass::run(compiler::ir::core::Module& module) {
  const auto before = detail::moduleFingerprint(module);
  for (auto& function : module.functions()) {
    runArrayAccessSimplify(function);
  }
  return before != detail::moduleFingerprint(module);
}

}  // namespace compiler::midend::pass
