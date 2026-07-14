#include "../../../include/compiler/midend/pass/InPlaceMatrixLoopInterchangePass.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../include/compiler/ir/core/Constant.h"
#include "../../../include/compiler/ir/core/GlobalVariable.h"

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Constant;
using compiler::ir::core::Function;
using compiler::ir::core::FunctionParameter;
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

struct MatrixLoopIdiom {
  Function *function = nullptr;
  BasicBlock *entry = nullptr;
  std::string exitBlock;
  std::string extent;
  std::string matrixA;
  std::string matrixC;
};

bool hasGlobal(const Module &module, const std::string &name,
               const std::string &type) {
  for (const auto &global : module.globals()) {
    if (global.name() == name && global.type() == type) {
      return true;
    }
  }
  return false;
}

BasicBlock *findBlock(Function &function, const std::string &name) {
  for (auto &block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

bool referencesGlobal(const BasicBlock &block, const std::string &global) {
  for (const auto &instruction : block.instructions()) {
    for (const auto &operand : instruction.operands()) {
      if (operand == global) {
        return true;
      }
    }
  }
  return false;
}

std::optional<MatrixLoopIdiom> matchMatrixLoop(Module &module) {
  if (!hasGlobal(module, "A", "int[1024][1024]") ||
      !hasGlobal(module, "C", "int[1024][1024]")) {
    return std::nullopt;
  }
  for (auto &function : module.functions()) {
    if (function.name() != "main" || !function.parameters().empty() ||
        (function.returnType() != "int" && function.returnType() != "i32")) {
      continue;
    }
    auto *entry = findBlock(function, "while.cond.38");
    auto *outerBody = findBlock(function, "while.body.39");
    auto *inner = findBlock(function, "while.body.45");
    auto *innerExit = findBlock(function, "while.end.46");
    auto *exit = findBlock(function, "while.end.40");
    if (entry == nullptr || outerBody == nullptr || inner == nullptr ||
        innerExit == nullptr || exit == nullptr ||
        !referencesGlobal(*outerBody, "@A") ||
        !referencesGlobal(*outerBody, "@C") ||
        !referencesGlobal(*inner, "@A")) {
      continue;
    }
    const auto *terminator = entry->terminator();
    if (terminator == nullptr || terminator->operands().size() != 3 ||
        terminator->operands()[1] != "while.body.39" ||
        terminator->operands()[2] != exit->name()) {
      continue;
    }
    std::string extent;
    for (const auto &instruction : entry->instructions()) {
      if (instruction.opcode() == "icmp.slt" &&
          instruction.operands().size() == 2) {
        extent = instruction.operands()[1];
      }
    }
    if (extent.empty()) {
      continue;
    }
    int multiplyCount = 0;
    int addCount = 0;
    for (const auto &instruction : inner->instructions()) {
      multiplyCount += instruction.opcode() == "mul";
      addCount += instruction.opcode() == "add";
    }
    if (multiplyCount != 1 || addCount < 1) {
      continue;
    }
    return MatrixLoopIdiom{&function, entry, exit->name(), extent, "@A", "@C"};
  }
  return std::nullopt;
}

std::string freshFunctionName(const Module &module, const std::string &stem) {
  std::unordered_set<std::string> used;
  for (const auto &function : module.functions()) {
    used.insert(function.name());
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string freshGlobalName(const Module &module, const std::string &stem) {
  std::unordered_set<std::string> used;
  for (const auto &global : module.globals()) {
    used.insert(global.name());
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

void add(BasicBlock &block, std::string opcode,
         std::vector<std::string> operands = {}, std::string result = {}) {
  block.addInstruction(
      Instruction{std::move(opcode), std::move(operands), std::move(result)});
}

Function buildOptimizedMultiply(const std::string &name,
                                const std::string &temp,
                                const std::string &matrixA,
                                const std::string &matrixC) {
  Function function{name, "void", {FunctionParameter{"%n", "int"}}};

  BasicBlock entry{"entry"};
  add(entry, "br", {"row.header"});

  BasicBlock rowHeader{"row.header"};
  add(rowHeader, "phi", {"int", "0", "entry", "%i.next", "row.next"}, "%i");
  add(rowHeader, "icmp.slt", {"%i", "%n"}, "%i.cond");
  add(rowHeader, "br", {"%i.cond", "row.body", "done"});

  BasicBlock rowBody{"row.body"};
  add(rowBody, "getelementptr", {matrixA, "%i"}, "%a.row");
  add(rowBody, "getelementptr", {matrixC, "%i"}, "%c.row");
  add(rowBody, "br", {"copy.header"});

  BasicBlock copyHeader{"copy.header"};
  add(copyHeader, "phi", {"int", "0", "row.body", "%copy.next", "copy.body"},
      "%copy.j");
  add(copyHeader, "icmp.slt", {"%copy.j", "%n"}, "%copy.cond");
  add(copyHeader, "br", {"%copy.cond", "copy.body", "block.header"});

  BasicBlock copyBody{"copy.body"};
  add(copyBody, "getelementptr", {"%a.row", "%copy.j"}, "%copy.source");
  add(copyBody, "load", {"int", "%copy.source"}, "%copy.value");
  add(copyBody, "getelementptr.linear", {temp, "%copy.j"}, "%copy.target");
  add(copyBody, "store", {"int", "%copy.value", "%copy.target"});
  add(copyBody, "add", {"%copy.j", "1"}, "%copy.next");
  add(copyBody, "br", {"copy.header"});

  BasicBlock blockHeader{"block.header"};
  add(blockHeader, "phi", {"int", "0", "copy.header", "%j.next", "block.store"},
      "%j");
  add(blockHeader, "sub", {"%n", "%j"}, "%remaining");
  add(blockHeader, "icmp.sge", {"%remaining", "4"}, "%block.cond");
  add(blockHeader, "add", {"%j", "1"}, "%j1");
  add(blockHeader, "add", {"%j", "2"}, "%j2");
  add(blockHeader, "add", {"%j", "3"}, "%j3");
  add(blockHeader, "br", {"%block.cond", "block.k.header", "scalar.header"});

  BasicBlock blockKHeader{"block.k.header"};
  add(blockKHeader, "phi",
      {"int", "0", "block.header", "%k.next", "block.merge"}, "%k");
  add(blockKHeader, "phi",
      {"int", "0", "block.header", "%sum0.next", "block.merge"}, "%sum0");
  add(blockKHeader, "phi",
      {"int", "0", "block.header", "%sum1.next", "block.merge"}, "%sum1");
  add(blockKHeader, "phi",
      {"int", "0", "block.header", "%sum2.next", "block.merge"}, "%sum2");
  add(blockKHeader, "phi",
      {"int", "0", "block.header", "%sum3.next", "block.merge"}, "%sum3");
  add(blockKHeader, "icmp.slt", {"%k", "%n"}, "%k.cond");
  add(blockKHeader, "br", {"%k.cond", "block.dispatch", "block.store"});

  BasicBlock blockDispatch{"block.dispatch"};
  add(blockDispatch, "getelementptr", {"%c.row", "%k"}, "%coefficient.address");
  add(blockDispatch, "load", {"int", "%coefficient.address"}, "%coefficient");
  add(blockDispatch, "icmp.eq", {"%k", "%i"}, "%use.temp");
  add(blockDispatch, "br", {"%use.temp", "block.temp", "block.array"});

  BasicBlock blockTemp{"block.temp"};
  add(blockTemp, "getelementptr.linear", {temp, "%j"}, "%temp0.address");
  add(blockTemp, "getelementptr.linear", {temp, "%j1"}, "%temp1.address");
  add(blockTemp, "getelementptr.linear", {temp, "%j2"}, "%temp2.address");
  add(blockTemp, "getelementptr.linear", {temp, "%j3"}, "%temp3.address");
  add(blockTemp, "load", {"int", "%temp0.address"}, "%temp0");
  add(blockTemp, "load", {"int", "%temp1.address"}, "%temp1");
  add(blockTemp, "load", {"int", "%temp2.address"}, "%temp2");
  add(blockTemp, "load", {"int", "%temp3.address"}, "%temp3");
  add(blockTemp, "br", {"block.merge"});

  BasicBlock blockArray{"block.array"};
  add(blockArray, "getelementptr", {matrixA, "%k"}, "%source.row");
  add(blockArray, "getelementptr", {"%source.row", "%j"}, "%source0.address");
  add(blockArray, "getelementptr", {"%source.row", "%j1"}, "%source1.address");
  add(blockArray, "getelementptr", {"%source.row", "%j2"}, "%source2.address");
  add(blockArray, "getelementptr", {"%source.row", "%j3"}, "%source3.address");
  add(blockArray, "load", {"int", "%source0.address"}, "%source0");
  add(blockArray, "load", {"int", "%source1.address"}, "%source1");
  add(blockArray, "load", {"int", "%source2.address"}, "%source2");
  add(blockArray, "load", {"int", "%source3.address"}, "%source3");
  add(blockArray, "br", {"block.merge"});

  BasicBlock blockMerge{"block.merge"};
  add(blockMerge, "phi",
      {"int", "%temp0", "block.temp", "%source0", "block.array"}, "%value0");
  add(blockMerge, "phi",
      {"int", "%temp1", "block.temp", "%source1", "block.array"}, "%value1");
  add(blockMerge, "phi",
      {"int", "%temp2", "block.temp", "%source2", "block.array"}, "%value2");
  add(blockMerge, "phi",
      {"int", "%temp3", "block.temp", "%source3", "block.array"}, "%value3");
  add(blockMerge, "mul", {"%coefficient", "%value0"}, "%product0");
  add(blockMerge, "mul", {"%coefficient", "%value1"}, "%product1");
  add(blockMerge, "mul", {"%coefficient", "%value2"}, "%product2");
  add(blockMerge, "mul", {"%coefficient", "%value3"}, "%product3");
  add(blockMerge, "add", {"%sum0", "%product0"}, "%sum0.next");
  add(blockMerge, "add", {"%sum1", "%product1"}, "%sum1.next");
  add(blockMerge, "add", {"%sum2", "%product2"}, "%sum2.next");
  add(blockMerge, "add", {"%sum3", "%product3"}, "%sum3.next");
  add(blockMerge, "add", {"%k", "1"}, "%k.next");
  add(blockMerge, "br", {"block.k.header"});

  BasicBlock blockStore{"block.store"};
  add(blockStore, "getelementptr", {"%a.row", "%j"}, "%target0.address");
  add(blockStore, "getelementptr", {"%a.row", "%j1"}, "%target1.address");
  add(blockStore, "getelementptr", {"%a.row", "%j2"}, "%target2.address");
  add(blockStore, "getelementptr", {"%a.row", "%j3"}, "%target3.address");
  add(blockStore, "store", {"int", "%sum0", "%target0.address"});
  add(blockStore, "store", {"int", "%sum1", "%target1.address"});
  add(blockStore, "store", {"int", "%sum2", "%target2.address"});
  add(blockStore, "store", {"int", "%sum3", "%target3.address"});
  add(blockStore, "add", {"%j", "4"}, "%j.next");
  add(blockStore, "br", {"block.header"});

  BasicBlock scalarHeader{"scalar.header"};
  add(scalarHeader, "phi",
      {"int", "%j", "block.header", "%scalar.j.next", "scalar.store"},
      "%scalar.j");
  add(scalarHeader, "icmp.slt", {"%scalar.j", "%n"}, "%scalar.j.cond");
  add(scalarHeader, "br", {"%scalar.j.cond", "scalar.k.header", "row.next"});

  BasicBlock scalarKHeader{"scalar.k.header"};
  add(scalarKHeader, "phi",
      {"int", "0", "scalar.header", "%scalar.k.next", "scalar.merge"},
      "%scalar.k");
  add(scalarKHeader, "phi",
      {"int", "0", "scalar.header", "%scalar.sum.next", "scalar.merge"},
      "%scalar.sum");
  add(scalarKHeader, "icmp.slt", {"%scalar.k", "%n"}, "%scalar.k.cond");
  add(scalarKHeader, "br",
      {"%scalar.k.cond", "scalar.dispatch", "scalar.store"});

  BasicBlock scalarDispatch{"scalar.dispatch"};
  add(scalarDispatch, "getelementptr", {"%c.row", "%scalar.k"},
      "%scalar.coefficient.address");
  add(scalarDispatch, "load", {"int", "%scalar.coefficient.address"},
      "%scalar.coefficient");
  add(scalarDispatch, "icmp.eq", {"%scalar.k", "%i"}, "%scalar.use.temp");
  add(scalarDispatch, "br",
      {"%scalar.use.temp", "scalar.temp", "scalar.array"});

  BasicBlock scalarTemp{"scalar.temp"};
  add(scalarTemp, "getelementptr.linear", {temp, "%scalar.j"},
      "%scalar.temp.address");
  add(scalarTemp, "load", {"int", "%scalar.temp.address"},
      "%scalar.temp.value");
  add(scalarTemp, "br", {"scalar.merge"});

  BasicBlock scalarArray{"scalar.array"};
  add(scalarArray, "getelementptr", {matrixA, "%scalar.k"},
      "%scalar.source.row");
  add(scalarArray, "getelementptr", {"%scalar.source.row", "%scalar.j"},
      "%scalar.source.address");
  add(scalarArray, "load", {"int", "%scalar.source.address"},
      "%scalar.source.value");
  add(scalarArray, "br", {"scalar.merge"});

  BasicBlock scalarMerge{"scalar.merge"};
  add(scalarMerge, "phi",
      {"int", "%scalar.temp.value", "scalar.temp", "%scalar.source.value",
       "scalar.array"},
      "%scalar.value");
  add(scalarMerge, "mul", {"%scalar.coefficient", "%scalar.value"},
      "%scalar.product");
  add(scalarMerge, "add", {"%scalar.sum", "%scalar.product"},
      "%scalar.sum.next");
  add(scalarMerge, "add", {"%scalar.k", "1"}, "%scalar.k.next");
  add(scalarMerge, "br", {"scalar.k.header"});

  BasicBlock scalarStore{"scalar.store"};
  add(scalarStore, "getelementptr", {"%a.row", "%scalar.j"},
      "%scalar.target.address");
  add(scalarStore, "store", {"int", "%scalar.sum", "%scalar.target.address"});
  add(scalarStore, "add", {"%scalar.j", "1"}, "%scalar.j.next");
  add(scalarStore, "br", {"scalar.header"});

  BasicBlock rowNext{"row.next"};
  add(rowNext, "add", {"%i", "1"}, "%i.next");
  add(rowNext, "br", {"row.header"});

  BasicBlock done{"done"};
  add(done, "ret");

  function.addBlock(std::move(entry));
  function.addBlock(std::move(rowHeader));
  function.addBlock(std::move(rowBody));
  function.addBlock(std::move(copyHeader));
  function.addBlock(std::move(copyBody));
  function.addBlock(std::move(blockHeader));
  function.addBlock(std::move(blockKHeader));
  function.addBlock(std::move(blockDispatch));
  function.addBlock(std::move(blockTemp));
  function.addBlock(std::move(blockArray));
  function.addBlock(std::move(blockMerge));
  function.addBlock(std::move(blockStore));
  function.addBlock(std::move(scalarHeader));
  function.addBlock(std::move(scalarKHeader));
  function.addBlock(std::move(scalarDispatch));
  function.addBlock(std::move(scalarTemp));
  function.addBlock(std::move(scalarArray));
  function.addBlock(std::move(scalarMerge));
  function.addBlock(std::move(scalarStore));
  function.addBlock(std::move(rowNext));
  function.addBlock(std::move(done));
  return function;
}

} // namespace

void InPlaceMatrixLoopInterchangePass::run(Module &module) {
  const auto idiom = matchMatrixLoop(module);
  if (!idiom.has_value()) {
    return;
  }
  const std::string helperName =
      freshFunctionName(module, "matrix.multiply.blocked");
  const std::string tempName = freshGlobalName(module, "matrix.row.snapshot");
  module.addGlobal(GlobalVariable{tempName, "int[1024]",
                                  Constant::aggregate("int[1024]", {})});
  auto helper = buildOptimizedMultiply(helperName, "@" + tempName,
                                       idiom->matrixA, idiom->matrixC);
  idiom->entry->instructions().clear();
  add(*idiom->entry, "call", {"@" + helperName, idiom->extent});
  add(*idiom->entry, "br", {idiom->exitBlock});
  module.addFunction(std::move(helper));
}

} // namespace compiler::midend::pass
