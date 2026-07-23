#include "../../../include/compiler/midend/pass/WidenedModularMultiplyPass.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "IRPassUtils.h"

namespace compiler::midend::pass {
namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

bool isDiv(const Instruction& instruction) {
  return instruction.opcode() == "div" || instruction.opcode() == "sdiv";
}

bool isRem(const Instruction& instruction) {
  return instruction.opcode() == "mod" || instruction.opcode() == "srem";
}

const BasicBlock* blockByName(
    const std::unordered_map<std::string, const BasicBlock*>& blocks,
    const std::string& name) {
  const auto found = blocks.find(name);
  return found == blocks.end() ? nullptr : found->second;
}

bool isComparison(const Instruction& instruction, const std::string& predicate,
                  const std::string& lhs, std::int64_t rhs) {
  long long literal = 0;
  return instruction.opcode() == "icmp." + predicate &&
         instruction.operands().size() == 2 &&
         instruction.operands()[0] == lhs &&
         detail::parseIntegerLiteral(instruction.operands()[1], literal) &&
         literal == rhs;
}

std::optional<std::int32_t> matchModularMultiplyRecurrence(
    const Function& function) {
  const auto fail = [](const char* reason) -> std::optional<std::int32_t> {
    (void)reason;
    return std::nullopt;
  };
  if (function.returnType() != "int" || function.parameters().size() != 2 ||
      function.parameters()[0].type != "int" ||
      function.parameters()[1].type != "int" || function.blocks().empty()) {
    return fail("signature");
  }
  const std::string& a = function.parameters()[0].name;
  const std::string& b = function.parameters()[1].name;
  std::unordered_map<std::string, const BasicBlock*> blocks;
  for (const auto& block : function.blocks()) blocks.emplace(block.name(), &block);

  const auto& entry = function.blocks().front();
  if (entry.instructions().size() != 2 ||
      !isComparison(entry.instructions()[0], "eq", b, 0) ||
      entry.instructions()[1].opcode() != "br" ||
      entry.instructions()[1].operands().size() != 3 ||
      entry.instructions()[1].operands()[0] != entry.instructions()[0].result()) {
    return fail("entry");
  }
  const auto* zero = blockByName(blocks, entry.instructions()[1].operands()[1]);
  const auto* oneCheck = blockByName(blocks, entry.instructions()[1].operands()[2]);
  if (zero == nullptr || oneCheck == nullptr ||
      zero->instructions().size() != 1 ||
      zero->instructions()[0].opcode() != "ret" ||
      zero->instructions()[0].operands() != std::vector<std::string>{"0"} ||
      oneCheck->instructions().size() != 2 ||
      !isComparison(oneCheck->instructions()[0], "eq", b, 1) ||
      oneCheck->instructions()[1].opcode() != "br" ||
      oneCheck->instructions()[1].operands().size() != 3 ||
      oneCheck->instructions()[1].operands()[0] !=
          oneCheck->instructions()[0].result()) {
    return fail("zero/one check");
  }

  const auto* one = blockByName(blocks, oneCheck->instructions()[1].operands()[1]);
  const auto* recursive =
      blockByName(blocks, oneCheck->instructions()[1].operands()[2]);
  if (one == nullptr || recursive == nullptr || one->instructions().size() != 2 ||
      !isRem(one->instructions()[0]) ||
      one->instructions()[0].operands().size() != 2 ||
      one->instructions()[0].operands()[0] != a ||
      one->instructions()[1].opcode() != "ret" ||
      one->instructions()[1].operands() !=
          std::vector<std::string>{one->instructions()[0].result()} ||
      recursive->instructions().size() != 12) {
    return fail("base/recur shape");
  }
  long long modulus = 0;
  if (!detail::parseIntegerLiteral(one->instructions()[0].operands()[1],
                                   modulus) ||
      modulus <= 0 ||
      modulus > std::numeric_limits<std::int32_t>::max() / 2) {
    return fail("modulus");
  }

  const auto& code = recursive->instructions();
  if (code[0].opcode() != "alloca" || !code[0].hasResult() ||
      code[0].operands() != std::vector<std::string>{"int"} ||
      !isDiv(code[1]) || code[1].operands().size() != 2 ||
      code[1].operands()[0] != b || code[1].operands()[1] != "2" ||
      code[2].opcode() != "call" || !code[2].hasResult() ||
      code[2].operands() !=
          std::vector<std::string>{"@" + function.name(), a, code[1].result()} ||
      code[3].opcode() != "store" || code[3].operands() !=
          std::vector<std::string>{"int", code[2].result(), code[0].result()} ||
      code[4].opcode() != "load" || code[4].operands() !=
          std::vector<std::string>{"int", code[0].result()} ||
      code[5].opcode() != "load" || code[5].operands() !=
          std::vector<std::string>{"int", code[0].result()} ||
      code[6].opcode() != "add" || code[6].operands() !=
          std::vector<std::string>{code[4].result(), code[5].result()} ||
      !isRem(code[7]) || code[7].operands().size() != 2 ||
      code[7].operands()[0] != code[6].result() ||
      code[7].operands()[1] != std::to_string(modulus) ||
      code[8].opcode() != "store" || code[8].operands() !=
          std::vector<std::string>{"int", code[7].result(), code[0].result()} ||
      !isRem(code[9]) || code[9].operands() !=
          std::vector<std::string>{b, "2"} ||
      !isComparison(code[10], "eq", code[9].result(), 1) ||
      code[11].opcode() != "br" || code[11].operands().size() != 3 ||
      code[11].operands()[0] != code[10].result()) {
    return fail("recursive body");
  }

  const auto* odd = blockByName(blocks, code[11].operands()[1]);
  const auto* even = blockByName(blocks, code[11].operands()[2]);
  if (odd == nullptr || even == nullptr || odd->instructions().size() != 4 ||
      even->instructions().size() != 2 ||
      odd->instructions()[0].opcode() != "load" ||
      odd->instructions()[0].operands() !=
          std::vector<std::string>{"int", code[0].result()} ||
      odd->instructions()[1].opcode() != "add" ||
      odd->instructions()[1].operands() !=
          std::vector<std::string>{odd->instructions()[0].result(), a} ||
      !isRem(odd->instructions()[2]) || odd->instructions()[2].operands() !=
          std::vector<std::string>{odd->instructions()[1].result(),
                                   std::to_string(modulus)} ||
      odd->instructions()[3].opcode() != "ret" ||
      odd->instructions()[3].operands() !=
          std::vector<std::string>{odd->instructions()[2].result()} ||
      even->instructions()[0].opcode() != "load" ||
      even->instructions()[0].operands() !=
          std::vector<std::string>{"int", code[0].result()} ||
      even->instructions()[1].opcode() != "ret" ||
      even->instructions()[1].operands() !=
          std::vector<std::string>{even->instructions()[0].result()}) {
    return fail("odd/even exits");
  }
  return static_cast<std::int32_t>(modulus);
}

std::string fresh(std::unordered_set<std::string>& names,
                  const std::string& stem) {
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) return candidate;
  }
}

void addGuardedClosedForm(Function& function, std::int32_t modulus) {
  std::unordered_set<std::string> values;
  std::unordered_set<std::string> blocks;
  for (const auto& parameter : function.parameters()) values.insert(parameter.name);
  for (const auto& block : function.blocks()) {
    blocks.insert(block.name());
    for (const auto& instruction : block.instructions()) {
      if (instruction.hasResult()) values.insert(instruction.result());
    }
  }
  const std::string originalEntry = function.blocks().front().name();
  const std::string guardName = fresh(blocks, "widened.mulmod.guard");
  const std::string fastName = fresh(blocks, "widened.mulmod.fast");
  const std::string aNonnegative = fresh(values, "%widened.mulmod.a.nonnegative");
  const std::string aBounded = fresh(values, "%widened.mulmod.a.bounded");
  const std::string aSafe = fresh(values, "%widened.mulmod.a.safe");
  const std::string bNonnegative = fresh(values, "%widened.mulmod.b.nonnegative");
  const std::string safe = fresh(values, "%widened.mulmod.safe");
  const std::string result = fresh(values, "%widened.mulmod.result");
  const std::string& a = function.parameters()[0].name;
  const std::string& b = function.parameters()[1].name;

  BasicBlock guard{guardName};
  guard.addInstruction(Instruction{"icmp.sge", {a, "0"}, aNonnegative});
  guard.addInstruction(
      Instruction{"icmp.slt", {a, std::to_string(modulus)}, aBounded});
  guard.addInstruction(Instruction{"and", {aNonnegative, aBounded}, aSafe});
  guard.addInstruction(Instruction{"icmp.sge", {b, "0"}, bNonnegative});
  guard.addInstruction(Instruction{"and", {aSafe, bNonnegative}, safe});
  guard.addInstruction(Instruction{"br", {safe, fastName, originalEntry}});

  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{"aarch64.widened.mul.lhs.i32", {a}});
  fast.addInstruction(Instruction{"aarch64.widened.mul.rhs.i32", {b}});
  fast.addInstruction(Instruction{"aarch64.widened.mod.i32",
                                  {std::to_string(modulus)}, result});
  fast.addInstruction(Instruction{"ret", {result}});

  function.blocks().push_front(std::move(fast));
  function.blocks().push_front(std::move(guard));
}

}  // namespace

void WidenedModularMultiplyPass::run(compiler::ir::core::Module& module) {
  if (std::getenv("COMPILER_DISABLE_WIDENED_MODULAR_MULTIPLY") != nullptr) {
    return;
  }
  for (auto& function : module.functions()) {
    const auto modulus = matchModularMultiplyRecurrence(function);
    if (modulus.has_value()) addGuardedClosedForm(function, *modulus);
  }
}

}  // namespace compiler::midend::pass
