#include "../../../include/compiler/midend/pass/ModularAffineCanonicalizationPass.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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
using compiler::ir::core::Module;

struct Definition {
  const Instruction* instruction = nullptr;
  std::size_t block = 0;
  std::size_t index = 0;
};

struct LinearForm {
  std::vector<std::string> order;
  std::unordered_map<std::string, int> coefficients;
  std::uint32_t constant = 0;
  std::size_t expandedOperators = 0;
  std::size_t removableOperators = 0;
};

struct Rewrite {
  std::size_t block = 0;
  std::size_t index = 0;
  std::vector<Instruction> instructions;
};

enum class IntegerKind { Unknown, I32, Other };

std::optional<std::uint32_t> i32Bits(const std::string& value);

bool isAddSub(const Instruction& instruction) {
  return instruction.opcode() == "add" || instruction.opcode() == "sub";
}

IntegerKind scalarKind(const std::string& type) {
  if (type == "int" || type == "bool") return IntegerKind::I32;
  if (type == "long" || type == "float") return IntegerKind::Other;
  return IntegerKind::Unknown;
}

IntegerKind valueKind(
    const std::string& value, const Function& function,
    const std::unordered_map<std::string, Definition>& definitions,
    std::unordered_map<std::string, IntegerKind>& memo,
    std::unordered_set<std::string>& visiting) {
  if (i32Bits(value).has_value()) return IntegerKind::I32;
  if (const auto cached = memo.find(value); cached != memo.end())
    return cached->second;
  for (const auto& parameter : function.parameters()) {
    if (parameter.name != value) continue;
    const auto kind = scalarKind(parameter.type);
    memo.emplace(value, kind);
    return kind;
  }
  const auto definition = definitions.find(value);
  if (definition == definitions.end() || !visiting.insert(value).second)
    return IntegerKind::Unknown;
  const auto& instruction = *definition->second.instruction;
  const auto& operands = instruction.operands();
  IntegerKind kind = IntegerKind::Unknown;
  if ((instruction.opcode() == "load" || instruction.opcode() == "phi") &&
      !operands.empty()) {
    kind = scalarKind(operands[0]);
  } else if ((instruction.opcode() == "zext" ||
              instruction.opcode() == "sext" ||
              instruction.opcode() == "trunc" ||
              instruction.opcode() == "cast") &&
             operands.size() == 2) {
    kind = scalarKind(operands[1]);
  } else if (instruction.opcode().rfind("icmp.", 0) == 0) {
    kind = IntegerKind::I32;
  } else if (instruction.opcode() == "neg" && operands.size() == 1) {
    kind = valueKind(operands[0], function, definitions, memo, visiting);
  } else if (instruction.opcode() == "select" && operands.size() == 3) {
    const auto trueKind =
        valueKind(operands[1], function, definitions, memo, visiting);
    const auto falseKind =
        valueKind(operands[2], function, definitions, memo, visiting);
    kind = trueKind == falseKind ? trueKind : IntegerKind::Unknown;
  } else if ((instruction.opcode() == "add" ||
              instruction.opcode() == "sub" ||
              instruction.opcode() == "mul" ||
              instruction.opcode() == "div" ||
              instruction.opcode() == "mod" ||
              instruction.opcode() == "sdiv" ||
              instruction.opcode() == "srem" ||
              instruction.opcode() == "and" ||
              instruction.opcode() == "or" ||
              instruction.opcode() == "xor" ||
              instruction.opcode() == "shl" ||
              instruction.opcode() == "ashr" ||
              instruction.opcode() == "lshr") &&
             operands.size() == 2) {
    const auto lhs =
        valueKind(operands[0], function, definitions, memo, visiting);
    const auto rhs =
        valueKind(operands[1], function, definitions, memo, visiting);
    if (lhs == IntegerKind::Other || rhs == IntegerKind::Other) {
      kind = IntegerKind::Other;
    } else if (lhs == IntegerKind::I32 && rhs == IntegerKind::I32) {
      kind = IntegerKind::I32;
    }
  }
  visiting.erase(value);
  memo[value] = kind;
  return kind;
}

std::optional<std::uint32_t> i32Bits(const std::string& value) {
  long long parsed = 0;
  if (!detail::parseIntegerLiteral(value, parsed) ||
      parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

std::string formatI32(std::uint32_t bits) {
  const std::int64_t signedValue =
      bits <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
          ? static_cast<std::int64_t>(bits)
          : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
  return std::to_string(signedValue);
}

void addLeaf(LinearForm& form, const std::string& value, int sign) {
  const auto found = form.coefficients.find(value);
  if (found == form.coefficients.end()) {
    form.order.push_back(value);
    form.coefficients.emplace(value, sign);
  } else {
    found->second += sign;
  }
}

bool flatten(const std::string& value, int sign,
             const std::unordered_map<std::string, Definition>& definitions,
             const std::unordered_map<std::string, std::size_t>& uses,
             LinearForm& form, std::unordered_set<std::string>& visiting) {
  if (const auto literal = i32Bits(value); literal.has_value()) {
    form.constant += sign > 0 ? *literal : std::uint32_t{0} - *literal;
    return true;
  }
  const auto definition = definitions.find(value);
  const auto use = uses.find(value);
  if (definition == definitions.end() || use == uses.end() ||
      !isAddSub(*definition->second.instruction)) {
    addLeaf(form, value, sign);
    return true;
  }
  if (form.expandedOperators >= 64 || !visiting.insert(value).second)
    return false;
  ++form.expandedOperators;
  if (use->second == 1) ++form.removableOperators;
  const auto& instruction = *definition->second.instruction;
  if (instruction.operands().size() != 2 ||
      !flatten(instruction.operands()[0], sign, definitions, uses, form,
               visiting) ||
      !flatten(instruction.operands()[1],
               instruction.opcode() == "sub" ? -sign : sign, definitions, uses,
               form, visiting)) {
    visiting.erase(value);
    return false;
  }
  visiting.erase(value);
  return true;
}

std::unordered_set<std::string> valueNames(const Function& function) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) names.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) names.insert(instruction.result());
    }
  }
  return names;
}

std::string freshValue(std::unordered_set<std::string>& names,
                       std::size_t& counter) {
  for (;;) {
    const auto candidate = "%mod.affine." + std::to_string(counter++);
    if (names.insert(candidate).second) return candidate;
  }
}

std::vector<Instruction> rebuild(const LinearForm& form,
                                 const std::string& result,
                                 std::unordered_set<std::string>& names,
                                 std::size_t& counter) {
  std::vector<std::pair<std::string, int>> terms;
  for (const auto& value : form.order) {
    const auto coefficient = form.coefficients.at(value);
    if (coefficient != 0) terms.emplace_back(value, coefficient);
  }
  std::vector<Instruction> instructions;
  std::string current;
  bool constantUsed = false;
  for (auto& [value, coefficient] : terms) {
    if (coefficient > 0 && current.empty()) {
      current = value;
      --coefficient;
      break;
    }
  }
  if (current.empty() && form.constant != 0) {
    current = formatI32(form.constant);
    constantUsed = true;
  }
  if (current.empty()) current = "0";

  auto emit = [&](const std::string& opcode, const std::string& operand,
                  bool final) {
    const auto destination = final ? result : freshValue(names, counter);
    instructions.emplace_back(opcode,
                              std::vector<std::string>{current, operand},
                              destination);
    current = destination;
  };

  std::vector<std::pair<std::string, int>> operations;
  for (const auto& [value, coefficient] : terms) {
    for (int count = 0; count < coefficient; ++count) {
      operations.emplace_back(value, 1);
    }
    for (int count = 0; count > coefficient; --count) {
      operations.emplace_back(value, -1);
    }
  }
  if (!constantUsed && form.constant != 0) {
    operations.emplace_back(formatI32(form.constant), 1);
  }
  if (operations.empty()) {
    instructions.emplace_back("add", std::vector<std::string>{current, "0"},
                              result);
    return instructions;
  }
  for (std::size_t index = 0; index < operations.size(); ++index) {
    emit(operations[index].second > 0 ? "add" : "sub",
         operations[index].first, index + 1 == operations.size());
  }
  return instructions;
}

std::size_t canonicalize(Function& function) {
  std::unordered_map<std::string, Definition> definitions;
  std::unordered_map<std::string, std::size_t> uses;
  std::unordered_map<std::string, const Instruction*> uniqueUsers;
  for (std::size_t block = 0; block < function.blocks().size(); ++block) {
    const auto& instructions = function.blocks()[block].instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].hasResult()) {
        definitions.emplace(instructions[index].result(),
                            Definition{&instructions[index], block, index});
      }
      for (const auto& operand : instructions[index].operands()) {
        if (operand.empty() || operand.front() != '%') continue;
        ++uses[operand];
        uniqueUsers[operand] = &instructions[index];
      }
    }
  }

  auto names = valueNames(function);
  std::unordered_map<std::string, IntegerKind> valueKinds;
  std::size_t counter = 0;
  std::vector<Rewrite> rewrites;
  for (const auto& [value, definition] : definitions) {
    const auto& instruction = *definition.instruction;
    if (!isAddSub(instruction) || instruction.operands().size() != 2 ||
        uses[value] == 0) {
      continue;
    }
    std::unordered_set<std::string> typeVisiting;
    if (valueKind(value, function, definitions, valueKinds, typeVisiting) !=
        IntegerKind::I32) {
      continue;
    }
    const auto user = uniqueUsers.find(value);
    if (uses[value] == 1 && user != uniqueUsers.end() &&
        isAddSub(*user->second)) {
      continue;
    }
    LinearForm form;
    form.expandedOperators = 1;
    form.removableOperators = 1;
    std::unordered_set<std::string> visiting{value};
    if (!flatten(instruction.operands()[0], 1, definitions, uses, form,
                 visiting) ||
        !flatten(instruction.operands()[1],
                 instruction.opcode() == "sub" ? -1 : 1, definitions, uses,
                 form, visiting)) {
      continue;
    }
    std::size_t magnitude = 0;
    for (const auto& [unused, coefficient] : form.coefficients) {
      (void)unused;
      magnitude += static_cast<std::size_t>(
          coefficient < 0 ? -coefficient : coefficient);
    }
    if (magnitude > 64) continue;
    auto replacement = rebuild(form, value, names, counter);
    if (replacement.size() >= form.removableOperators) continue;
    rewrites.push_back(
        Rewrite{definition.block, definition.index, std::move(replacement)});
  }

  std::sort(rewrites.begin(), rewrites.end(), [](const Rewrite& lhs,
                                                 const Rewrite& rhs) {
    if (lhs.block != rhs.block) return lhs.block > rhs.block;
    return lhs.index > rhs.index;
  });
  for (auto& rewrite : rewrites) {
    auto& instructions = function.blocks()[rewrite.block].instructions();
    instructions.erase(instructions.begin() +
                       static_cast<std::ptrdiff_t>(rewrite.index));
    instructions.insert(
        instructions.begin() + static_cast<std::ptrdiff_t>(rewrite.index),
        std::make_move_iterator(rewrite.instructions.begin()),
        std::make_move_iterator(rewrite.instructions.end()));
  }
  return rewrites.size();
}

}  // namespace

void ModularAffineCanonicalizationPass::run(Module& module) {
  if (std::getenv("COMPILER_DISABLE_MODULAR_AFFINE_CANONICALIZATION") !=
      nullptr) {
    return;
  }
  std::size_t transformed = 0;
  for (auto& function : module.functions())
    transformed += canonicalize(function);
  if (std::getenv("COMPILER_MODULAR_AFFINE_STATS") != nullptr) {
    std::cerr << "[modular-affine] transformed=" << transformed << '\n';
  }
}

}  // namespace compiler::midend::pass
