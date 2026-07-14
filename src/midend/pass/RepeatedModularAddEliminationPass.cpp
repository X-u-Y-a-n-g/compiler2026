#include "../../../include/compiler/midend/pass/RepeatedModularAddEliminationPass.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct ModularAddIdiom {
  std::string stateAddress;
  std::string boundDivisor;
  std::string increment;
  std::string modulus;
  std::vector<Instruction> originalEntry;
};

std::optional<std::int64_t> parseInteger(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

const Instruction *findInstruction(const BasicBlock &block,
                                   const std::string &opcode) {
  for (const auto &instruction : block.instructions()) {
    if (instruction.opcode() == opcode) {
      return &instruction;
    }
  }
  return nullptr;
}

std::vector<const Instruction *> findInstructions(const BasicBlock &block,
                                                  const std::string &opcode) {
  std::vector<const Instruction *> found;
  for (const auto &instruction : block.instructions()) {
    if (instruction.opcode() == opcode) {
      found.push_back(&instruction);
    }
  }
  return found;
}

std::optional<ModularAddIdiom> matchModularAddIdiom(const Function &function) {
  if (!function.parameters().empty() || function.blocks().size() != 4 ||
      (function.returnType() != "int" && function.returnType() != "i32")) {
    return std::nullopt;
  }
  const auto &entry = function.blocks()[0];
  const auto &header = function.blocks()[1];
  const auto &body = function.blocks()[2];
  const auto &exit = function.blocks()[3];
  if (entry.instructions().size() != 3 || header.instructions().size() != 4 ||
      body.instructions().size() != 4 || exit.instructions().size() != 3) {
    return std::nullopt;
  }

  const auto *initialLoad = findInstruction(entry, "load");
  const auto entryRemainders = findInstructions(entry, "srem");
  const auto *entryBranch = entry.terminator();
  const auto headerPhis = findInstructions(header, "phi");
  const auto *compare = findInstruction(header, "icmp.slt");
  const auto *headerBranch = header.terminator();
  const auto bodyAdds = findInstructions(body, "add");
  const auto bodyRemainders = findInstructions(body, "srem");
  const auto *bodyBranch = body.terminator();
  const auto exitRemainders = findInstructions(exit, "srem");
  const auto *store = findInstruction(exit, "store");
  const auto *ret = exit.terminator();
  if (initialLoad == nullptr || initialLoad->operands().size() != 2 ||
      entryRemainders.size() != 1 ||
      entryRemainders[0]->operands().size() != 2 || entryBranch == nullptr ||
      entryBranch->operands().size() != 1 || headerPhis.size() != 2 ||
      compare == nullptr || compare->operands().size() != 2 ||
      headerBranch == nullptr || headerBranch->operands().size() != 3 ||
      bodyAdds.size() != 2 || bodyRemainders.size() != 1 ||
      bodyRemainders[0]->operands().size() != 2 || bodyBranch == nullptr ||
      bodyBranch->operands().size() != 1 || exitRemainders.size() != 1 ||
      exitRemainders[0]->operands().size() != 2 || store == nullptr ||
      store->operands().size() != 3 || ret == nullptr ||
      ret->operands().size() != 1) {
    return std::nullopt;
  }

  const auto *boundInstruction = entryRemainders[0];
  if (initialLoad->operands()[0] != "int" ||
      boundInstruction->operands()[0] != initialLoad->result() ||
      entryBranch->operands()[0] != header.name() ||
      headerBranch->operands()[1] != body.name() ||
      headerBranch->operands()[2] != exit.name() ||
      bodyBranch->operands()[0] != header.name()) {
    return std::nullopt;
  }

  const Instruction *valuePhi = nullptr;
  const Instruction *counterPhi = nullptr;
  for (const auto *phi : headerPhis) {
    if (phi->operands().size() != 5) {
      return std::nullopt;
    }
    if (phi->operands()[1] == initialLoad->result() &&
        phi->operands()[2] == entry.name()) {
      valuePhi = phi;
    }
    if (phi->operands()[1] == "0" && phi->operands()[2] == entry.name()) {
      counterPhi = phi;
    }
  }
  if (valuePhi == nullptr || counterPhi == nullptr ||
      compare->operands()[0] != counterPhi->result() ||
      compare->operands()[1] != boundInstruction->result() ||
      headerBranch->operands()[0] != compare->result()) {
    return std::nullopt;
  }

  const Instruction *valueAdd = nullptr;
  const Instruction *counterAdd = nullptr;
  for (const auto *add : bodyAdds) {
    if (add->operands().size() != 2) {
      return std::nullopt;
    }
    if (add->operands()[0] == valuePhi->result()) {
      valueAdd = add;
    }
    if (add->operands()[0] == counterPhi->result() &&
        add->operands()[1] == "1") {
      counterAdd = add;
    }
  }
  const auto *modularValue = bodyRemainders[0];
  const auto *finalRemainder = exitRemainders[0];
  if (valueAdd == nullptr || counterAdd == nullptr ||
      modularValue->operands()[0] != valueAdd->result() ||
      finalRemainder->operands()[0] != valuePhi->result() ||
      modularValue->operands()[1] != finalRemainder->operands()[1] ||
      valuePhi->operands()[3] != modularValue->result() ||
      valuePhi->operands()[4] != body.name() ||
      counterPhi->operands()[3] != counterAdd->result() ||
      counterPhi->operands()[4] != body.name() ||
      store->operands()[0] != "int" ||
      store->operands()[1] != finalRemainder->result() ||
      store->operands()[2] != initialLoad->operands()[1] ||
      ret->operands()[0] != finalRemainder->result()) {
    return std::nullopt;
  }

  const auto divisor = parseInteger(boundInstruction->operands()[1]);
  const auto increment = parseInteger(valueAdd->operands()[1]);
  const auto modulus = parseInteger(finalRemainder->operands()[1]);
  constexpr auto kIntMax = std::numeric_limits<std::int32_t>::max();
  if (!divisor.has_value() || !increment.has_value() || !modulus.has_value() ||
      *divisor <= 0 || *increment < 0 || *modulus <= 1 || *divisor > kIntMax ||
      *increment > kIntMax || *modulus > kIntMax ||
      (*divisor - 1) * *increment > kIntMax) {
    return std::nullopt;
  }

  return ModularAddIdiom{initialLoad->operands()[1],
                         boundInstruction->operands()[1],
                         valueAdd->operands()[1], finalRemainder->operands()[1],
                         entry.instructions()};
}

std::string freshBlockName(const Function &function, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    names.insert(block.name());
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = stem + "." + std::to_string(index);
    if (names.find(candidate) == names.end()) {
      return candidate;
    }
  }
}

std::string freshValueName(const Function &function, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.hasResult()) {
        names.insert(instruction.result());
      }
    }
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = "%" + stem + "." + std::to_string(index);
    if (names.find(candidate) == names.end()) {
      return candidate;
    }
  }
}

void addClosedFormFastPath(Function &function, const ModularAddIdiom &idiom) {
  const std::string originalEntryName = function.blocks().front().name();
  const std::string fastName = freshBlockName(function, "modadd.fast");
  const std::string fallbackName = freshBlockName(function, "modadd.fallback");
  const std::string initialValue = freshValueName(function, "modadd.initial");
  const std::string bound = freshValueName(function, "modadd.bound");
  const std::string boundNonnegative =
      freshValueName(function, "modadd.bound.nonnegative");
  const std::string delta = freshValueName(function, "modadd.delta");
  const std::string safeLimit = freshValueName(function, "modadd.safe.limit");
  const std::string noOverflow = freshValueName(function, "modadd.no.overflow");
  const std::string useFastPath =
      freshValueName(function, "modadd.use.fast.path");
  const std::string advanced = freshValueName(function, "modadd.advanced");
  const std::string result = freshValueName(function, "modadd.result");

  for (auto &block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() != "phi") {
        rewritten.push_back(instruction);
        continue;
      }
      auto operands = instruction.operands();
      for (std::size_t index = 2; index < operands.size(); index += 2) {
        if (operands[index] == originalEntryName) {
          operands[index] = fallbackName;
        }
      }
      rewritten.emplace_back("phi", std::move(operands), instruction.result());
    }
    block.instructions() = std::move(rewritten);
  }

  auto &entry = function.blocks().front().instructions();
  entry.clear();
  entry.emplace_back("load",
                     std::vector<std::string>{"int", idiom.stateAddress},
                     initialValue);
  entry.emplace_back("srem",
                     std::vector<std::string>{initialValue, idiom.boundDivisor},
                     bound);
  entry.emplace_back("icmp.sge", std::vector<std::string>{bound, "0"},
                     boundNonnegative);
  entry.emplace_back("mul", std::vector<std::string>{bound, idiom.increment},
                     delta);
  entry.emplace_back("sub", std::vector<std::string>{"2147483647", delta},
                     safeLimit);
  entry.emplace_back("icmp.sle",
                     std::vector<std::string>{initialValue, safeLimit},
                     noOverflow);
  entry.emplace_back("and",
                     std::vector<std::string>{boundNonnegative, noOverflow},
                     useFastPath);
  entry.emplace_back(
      "br", std::vector<std::string>{useFastPath, fastName, fallbackName});

  BasicBlock fast{fastName};
  fast.addInstruction(Instruction{"add", {initialValue, delta}, advanced});
  fast.addInstruction(Instruction{"srem", {advanced, idiom.modulus}, result});
  fast.addInstruction(
      Instruction{"store", {"int", result, idiom.stateAddress}});
  fast.addInstruction(Instruction{"ret", {result}});
  BasicBlock fallback{fallbackName};
  fallback.instructions() = idiom.originalEntry;
  function.blocks().insert(function.blocks().begin() + 1, std::move(fast));
  function.blocks().insert(function.blocks().begin() + 2, std::move(fallback));
}

} // namespace

void RepeatedModularAddEliminationPass::run(
    compiler::ir::core::Module &module) {
  for (auto &function : module.functions()) {
    const auto idiom = matchModularAddIdiom(function);
    if (idiom.has_value()) {
      addClosedFormFastPath(function, *idiom);
    }
  }
}

} // namespace compiler::midend::pass
