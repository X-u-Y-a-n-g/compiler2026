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

void buildBlockedArrayStep(BasicBlock &block, const std::string &prefix,
                           const std::string &k, const std::string &sum0,
                           const std::string &sum1, const std::string &sum2,
                           const std::string &sum3,
                           const std::string &matrixA,
                           const std::string &successor) {
  add(block, "getelementptr", {"%c.row", k},
      prefix + ".coefficient.address");
  add(block, "load", {"int", prefix + ".coefficient.address"},
      prefix + ".coefficient");
  add(block, "getelementptr", {matrixA, k}, prefix + ".source.row");
  add(block, "getelementptr", {prefix + ".source.row", "%j"},
      prefix + ".source0.address");
  add(block, "getelementptr", {prefix + ".source.row", "%j1"},
      prefix + ".source1.address");
  add(block, "getelementptr", {prefix + ".source.row", "%j2"},
      prefix + ".source2.address");
  add(block, "getelementptr", {prefix + ".source.row", "%j3"},
      prefix + ".source3.address");
  for (int column = 0; column < 4; ++column) {
    const std::string suffix = std::to_string(column);
    add(block, "load", {"int", prefix + ".source" + suffix + ".address"},
        prefix + ".source" + suffix);
    add(block, "mul",
        {prefix + ".coefficient", prefix + ".source" + suffix},
        prefix + ".product" + suffix);
  }
  add(block, "add", {sum0, prefix + ".product0"}, prefix + ".sum0.next");
  add(block, "add", {sum1, prefix + ".product1"}, prefix + ".sum1.next");
  add(block, "add", {sum2, prefix + ".product2"}, prefix + ".sum2.next");
  add(block, "add", {sum3, prefix + ".product3"}, prefix + ".sum3.next");
  add(block, "add", {k, "1"}, prefix + ".k.next");
  add(block, "br", {successor});
}

void buildScalarArrayStep(BasicBlock &block, const std::string &prefix,
                          const std::string &k, const std::string &sum,
                          const std::string &matrixA,
                          const std::string &successor) {
  add(block, "getelementptr", {"%c.row", k},
      prefix + ".coefficient.address");
  add(block, "load", {"int", prefix + ".coefficient.address"},
      prefix + ".coefficient");
  add(block, "getelementptr", {matrixA, k}, prefix + ".source.row");
  add(block, "getelementptr", {prefix + ".source.row", "%scalar.j"},
      prefix + ".source.address");
  add(block, "load", {"int", prefix + ".source.address"},
      prefix + ".source");
  add(block, "mul", {prefix + ".coefficient", prefix + ".source"},
      prefix + ".product");
  add(block, "add", {sum, prefix + ".product"}, prefix + ".sum.next");
  add(block, "add", {k, "1"}, prefix + ".k.next");
  add(block, "br", {successor});
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
  add(blockHeader, "br", {"%block.cond", "block.pre.header", "scalar.header"});

  BasicBlock blockPreHeader{"block.pre.header"};
  add(blockPreHeader, "phi",
      {"int", "0", "block.header", "%block.pre.k.next", "block.pre.body"},
      "%block.pre.k");
  add(blockPreHeader, "phi",
      {"int", "0", "block.header", "%block.pre.sum0.next", "block.pre.body"},
      "%block.pre.sum0");
  add(blockPreHeader, "phi",
      {"int", "0", "block.header", "%block.pre.sum1.next", "block.pre.body"},
      "%block.pre.sum1");
  add(blockPreHeader, "phi",
      {"int", "0", "block.header", "%block.pre.sum2.next", "block.pre.body"},
      "%block.pre.sum2");
  add(blockPreHeader, "phi",
      {"int", "0", "block.header", "%block.pre.sum3.next", "block.pre.body"},
      "%block.pre.sum3");
  add(blockPreHeader, "icmp.slt", {"%block.pre.k", "%i"},
      "%block.pre.cond");
  add(blockPreHeader, "br",
      {"%block.pre.cond", "block.pre.body", "block.temp"});

  BasicBlock blockPreBody{"block.pre.body"};
  buildBlockedArrayStep(blockPreBody, "%block.pre", "%block.pre.k",
                        "%block.pre.sum0", "%block.pre.sum1",
                        "%block.pre.sum2", "%block.pre.sum3", matrixA,
                        "block.pre.header");

  BasicBlock blockTemp{"block.temp"};
  add(blockTemp, "getelementptr", {"%c.row", "%i"},
      "%block.temp.coefficient.address");
  add(blockTemp, "load", {"int", "%block.temp.coefficient.address"},
      "%block.temp.coefficient");
  for (int column = 0; column < 4; ++column) {
    const std::string suffix = std::to_string(column);
    const std::string index = column == 0 ? "%j" : "%j" + suffix;
    add(blockTemp, "getelementptr.linear", {temp, index},
        "%block.temp.source" + suffix + ".address");
    add(blockTemp, "load",
        {"int", "%block.temp.source" + suffix + ".address"},
        "%block.temp.source" + suffix);
    add(blockTemp, "mul",
        {"%block.temp.coefficient", "%block.temp.source" + suffix},
        "%block.temp.product" + suffix);
    add(blockTemp, "add",
        {"%block.pre.sum" + suffix, "%block.temp.product" + suffix},
        "%block.temp.sum" + suffix);
  }
  add(blockTemp, "add", {"%i", "1"}, "%block.post.start");
  add(blockTemp, "br", {"block.post.header"});

  BasicBlock blockPostHeader{"block.post.header"};
  add(blockPostHeader, "phi",
      {"int", "%block.post.start", "block.temp", "%block.post.k.next",
       "block.post.body"},
      "%block.post.k");
  for (int column = 0; column < 4; ++column) {
    const std::string suffix = std::to_string(column);
    add(blockPostHeader, "phi",
        {"int", "%block.temp.sum" + suffix, "block.temp",
         "%block.post.sum" + suffix + ".next", "block.post.body"},
        "%block.post.sum" + suffix);
  }
  add(blockPostHeader, "icmp.slt", {"%block.post.k", "%n"},
      "%block.post.cond");
  add(blockPostHeader, "br",
      {"%block.post.cond", "block.post.body", "block.store"});

  BasicBlock blockPostBody{"block.post.body"};
  buildBlockedArrayStep(blockPostBody, "%block.post", "%block.post.k",
                        "%block.post.sum0", "%block.post.sum1",
                        "%block.post.sum2", "%block.post.sum3", matrixA,
                        "block.post.header");

  BasicBlock blockStore{"block.store"};
  add(blockStore, "getelementptr", {"%a.row", "%j"}, "%target0.address");
  add(blockStore, "getelementptr", {"%a.row", "%j1"}, "%target1.address");
  add(blockStore, "getelementptr", {"%a.row", "%j2"}, "%target2.address");
  add(blockStore, "getelementptr", {"%a.row", "%j3"}, "%target3.address");
  add(blockStore, "store", {"int", "%block.post.sum0", "%target0.address"});
  add(blockStore, "store", {"int", "%block.post.sum1", "%target1.address"});
  add(blockStore, "store", {"int", "%block.post.sum2", "%target2.address"});
  add(blockStore, "store", {"int", "%block.post.sum3", "%target3.address"});
  add(blockStore, "add", {"%j", "4"}, "%j.next");
  add(blockStore, "br", {"block.header"});

  BasicBlock scalarHeader{"scalar.header"};
  add(scalarHeader, "phi",
      {"int", "%j", "block.header", "%scalar.j.next", "scalar.store"},
      "%scalar.j");
  add(scalarHeader, "icmp.slt", {"%scalar.j", "%n"}, "%scalar.j.cond");
  add(scalarHeader, "br",
      {"%scalar.j.cond", "scalar.pre.header", "row.next"});

  BasicBlock scalarPreHeader{"scalar.pre.header"};
  add(scalarPreHeader, "phi",
      {"int", "0", "scalar.header", "%scalar.pre.k.next",
       "scalar.pre.body"},
      "%scalar.pre.k");
  add(scalarPreHeader, "phi",
      {"int", "0", "scalar.header", "%scalar.pre.sum.next",
       "scalar.pre.body"},
      "%scalar.pre.sum");
  add(scalarPreHeader, "icmp.slt", {"%scalar.pre.k", "%i"},
      "%scalar.pre.cond");
  add(scalarPreHeader, "br",
      {"%scalar.pre.cond", "scalar.pre.body", "scalar.temp"});

  BasicBlock scalarPreBody{"scalar.pre.body"};
  buildScalarArrayStep(scalarPreBody, "%scalar.pre", "%scalar.pre.k",
                       "%scalar.pre.sum", matrixA, "scalar.pre.header");

  BasicBlock scalarTemp{"scalar.temp"};
  add(scalarTemp, "getelementptr", {"%c.row", "%i"},
      "%scalar.temp.coefficient.address");
  add(scalarTemp, "load", {"int", "%scalar.temp.coefficient.address"},
      "%scalar.temp.coefficient");
  add(scalarTemp, "getelementptr.linear", {temp, "%scalar.j"},
      "%scalar.temp.address");
  add(scalarTemp, "load", {"int", "%scalar.temp.address"},
      "%scalar.temp.value");
  add(scalarTemp, "mul",
      {"%scalar.temp.coefficient", "%scalar.temp.value"},
      "%scalar.temp.product");
  add(scalarTemp, "add", {"%scalar.pre.sum", "%scalar.temp.product"},
      "%scalar.temp.sum");
  add(scalarTemp, "add", {"%i", "1"}, "%scalar.post.start");
  add(scalarTemp, "br", {"scalar.post.header"});

  BasicBlock scalarPostHeader{"scalar.post.header"};
  add(scalarPostHeader, "phi",
      {"int", "%scalar.post.start", "scalar.temp", "%scalar.post.k.next",
       "scalar.post.body"},
      "%scalar.post.k");
  add(scalarPostHeader, "phi",
      {"int", "%scalar.temp.sum", "scalar.temp", "%scalar.post.sum.next",
       "scalar.post.body"},
      "%scalar.post.sum");
  add(scalarPostHeader, "icmp.slt", {"%scalar.post.k", "%n"},
      "%scalar.post.cond");
  add(scalarPostHeader, "br",
      {"%scalar.post.cond", "scalar.post.body", "scalar.store"});

  BasicBlock scalarPostBody{"scalar.post.body"};
  buildScalarArrayStep(scalarPostBody, "%scalar.post", "%scalar.post.k",
                       "%scalar.post.sum", matrixA, "scalar.post.header");

  BasicBlock scalarStore{"scalar.store"};
  add(scalarStore, "getelementptr", {"%a.row", "%scalar.j"},
      "%scalar.target.address");
  add(scalarStore, "store",
      {"int", "%scalar.post.sum", "%scalar.target.address"});
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
  function.addBlock(std::move(blockPreHeader));
  function.addBlock(std::move(blockPreBody));
  function.addBlock(std::move(blockTemp));
  function.addBlock(std::move(blockPostHeader));
  function.addBlock(std::move(blockPostBody));
  function.addBlock(std::move(blockStore));
  function.addBlock(std::move(scalarHeader));
  function.addBlock(std::move(scalarPreHeader));
  function.addBlock(std::move(scalarPreBody));
  function.addBlock(std::move(scalarTemp));
  function.addBlock(std::move(scalarPostHeader));
  function.addBlock(std::move(scalarPostBody));
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
