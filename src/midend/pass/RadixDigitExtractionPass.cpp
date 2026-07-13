#include "../../../include/compiler/midend/pass/RadixDigitExtractionPass.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;

struct RadixDigitIdiom {
  std::vector<Instruction> originalEntry;
};

bool isIntegerParameter(const compiler::ir::core::FunctionParameter& parameter) {
  return parameter.type == "int" || parameter.type == "i32";
}

const Instruction* findInstruction(const BasicBlock& block, const std::string& opcode) {
  for (const auto& instruction : block.instructions()) {
    if (instruction.opcode() == opcode) {
      return &instruction;
    }
  }
  return nullptr;
}

std::optional<RadixDigitIdiom> matchRadixDigitIdiom(const Function& function) {
  if ((function.returnType() != "int" && function.returnType() != "i32") ||
      function.parameters().size() != 2 ||
      !isIntegerParameter(function.parameters()[0]) ||
      !isIntegerParameter(function.parameters()[1]) || function.blocks().size() != 4) {
    return std::nullopt;
  }
  const auto& entry = function.blocks()[0];
  const auto& header = function.blocks()[1];
  const auto& body = function.blocks()[2];
  const auto& exit = function.blocks()[3];
  if (entry.instructions().size() != 1 || entry.instructions()[0].opcode() != "br" ||
      entry.instructions()[0].operands().size() != 1 ||
      entry.instructions()[0].operands()[0] != header.name() ||
      header.instructions().size() != 4 || body.instructions().size() != 3 ||
      exit.instructions().size() != 2) {
    return std::nullopt;
  }

  std::vector<const Instruction*> phis;
  for (const auto& instruction : header.instructions()) {
    if (instruction.opcode() == "phi") {
      phis.push_back(&instruction);
    }
  }
  const auto* compare = findInstruction(header, "icmp.slt");
  const auto* headerBranch = header.terminator();
  const auto* division = findInstruction(body, "sdiv");
  const auto* increment = findInstruction(body, "add");
  const auto* bodyBranch = body.terminator();
  const auto* remainder = findInstruction(exit, "srem");
  const auto* returnInstruction = exit.terminator();
  if (phis.size() != 2 || compare == nullptr || headerBranch == nullptr ||
      division == nullptr || increment == nullptr || bodyBranch == nullptr ||
      remainder == nullptr || returnInstruction == nullptr ||
      compare->operands().size() != 2 || headerBranch->operands().size() != 3 ||
      division->operands().size() != 2 || increment->operands().size() != 2 ||
      bodyBranch->operands().size() != 1 || remainder->operands().size() != 2 ||
      returnInstruction->operands().size() != 1) {
    return std::nullopt;
  }

  const Instruction* valuePhi = nullptr;
  const Instruction* counterPhi = nullptr;
  for (const auto* phi : phis) {
    if (phi->operands().size() != 5) {
      return std::nullopt;
    }
    if (phi->operands()[1] == function.parameters()[0].name &&
        phi->operands()[2] == entry.name()) {
      valuePhi = phi;
    }
    if (phi->operands()[1] == "0" && phi->operands()[2] == entry.name()) {
      counterPhi = phi;
    }
  }
  if (valuePhi == nullptr || counterPhi == nullptr ||
      compare->operands()[0] != counterPhi->result() ||
      compare->operands()[1] != function.parameters()[1].name ||
      headerBranch->operands()[0] != compare->result() ||
      headerBranch->operands()[1] != body.name() ||
      headerBranch->operands()[2] != exit.name() ||
      division->operands()[0] != valuePhi->result() || division->operands()[1] != "16" ||
      increment->operands()[0] != counterPhi->result() || increment->operands()[1] != "1" ||
      bodyBranch->operands()[0] != header.name() ||
      remainder->operands()[0] != valuePhi->result() || remainder->operands()[1] != "16" ||
      returnInstruction->operands()[0] != remainder->result() ||
      valuePhi->operands()[3] != division->result() || valuePhi->operands()[4] != body.name() ||
      counterPhi->operands()[3] != increment->result() ||
      counterPhi->operands()[4] != body.name()) {
    return std::nullopt;
  }
  return RadixDigitIdiom{entry.instructions()};
}

std::string freshBlockName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& block : function.blocks()) {
    names.insert(block.name());
  }
  for (std::size_t index = 0;; ++index) {
    const std::string candidate = stem + "." + std::to_string(index);
    if (names.find(candidate) == names.end()) {
      return candidate;
    }
  }
}

std::string freshValueName(const Function& function, const std::string& stem) {
  std::unordered_set<std::string> names;
  for (const auto& parameter : function.parameters()) {
    names.insert(parameter.name);
  }
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
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

void addFastPath(Function& function, const RadixDigitIdiom& idiom) {
  const std::string originalEntryName = function.blocks().front().name();
  const std::string dispatchName = freshBlockName(function, "radix.dispatch");
  const std::string shiftName = freshBlockName(function, "radix.shift");
  const std::string zeroName = freshBlockName(function, "radix.zero");
  const std::string fallbackName = freshBlockName(function, "radix.fallback");
  const std::string valueNonnegative = freshValueName(function, "radix.value.nonnegative");
  const std::string positionNonnegative = freshValueName(function, "radix.position.nonnegative");
  const std::string useFastPath = freshValueName(function, "radix.use.fast.path");
  const std::string positionTooLarge = freshValueName(function, "radix.position.too.large");
  const std::string shiftAmount = freshValueName(function, "radix.shift.amount");
  const std::string shifted = freshValueName(function, "radix.shifted");
  const std::string digit = freshValueName(function, "radix.digit");

  for (auto& block : function.blocks()) {
    std::vector<Instruction> rewritten;
    rewritten.reserve(block.instructions().size());
    for (const auto& instruction : block.instructions()) {
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

  auto& entry = function.blocks().front().instructions();
  entry.clear();
  entry.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[0].name, "0"},
      valueNonnegative);
  entry.emplace_back(
      "icmp.sge", std::vector<std::string>{function.parameters()[1].name, "0"},
      positionNonnegative);
  entry.emplace_back("and", std::vector<std::string>{valueNonnegative, positionNonnegative},
                     useFastPath);
  entry.emplace_back("br", std::vector<std::string>{useFastPath, dispatchName, fallbackName});

  BasicBlock dispatch{dispatchName};
  dispatch.addInstruction(Instruction{
      "icmp.sge", {function.parameters()[1].name, "8"}, positionTooLarge});
  dispatch.addInstruction(Instruction{"br", {positionTooLarge, zeroName, shiftName}});

  BasicBlock shift{shiftName};
  shift.addInstruction(Instruction{"shl", {function.parameters()[1].name, "2"}, shiftAmount});
  shift.addInstruction(
      Instruction{"lshr", {function.parameters()[0].name, shiftAmount}, shifted});
  shift.addInstruction(Instruction{"and", {shifted, "15"}, digit});
  shift.addInstruction(Instruction{"ret", {digit}});

  BasicBlock zero{zeroName};
  zero.addInstruction(Instruction{"ret", {"0"}});
  BasicBlock fallback{fallbackName};
  fallback.instructions() = idiom.originalEntry;
  function.blocks().insert(function.blocks().begin() + 1, std::move(dispatch));
  function.blocks().insert(function.blocks().begin() + 2, std::move(shift));
  function.blocks().insert(function.blocks().begin() + 3, std::move(zero));
  function.blocks().insert(function.blocks().begin() + 4, std::move(fallback));
}

}  // namespace

void RadixDigitExtractionPass::run(compiler::ir::core::Module& module) {
  for (auto& function : module.functions()) {
    const auto idiom = matchRadixDigitIdiom(function);
    if (idiom.has_value()) {
      addFastPath(function, *idiom);
    }
  }
}

}  // namespace compiler::midend::pass
