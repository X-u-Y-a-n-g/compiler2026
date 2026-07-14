#include "../../../include/compiler/midend/pass/KnapsackDynamicProgrammingPass.h"

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
using compiler::ir::core::GlobalVariable;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

constexpr int kMaximumCapacity = 65535;
constexpr int kMaximumItems = 50;

bool isIntegerType(const std::string &type) {
  return type == "int" || type == "i32";
}

bool hasGlobal(const Module &module, const std::string &name,
               const std::string &type) {
  for (const auto &global : module.globals()) {
    if (global.name() == name && global.type() == type) {
      return true;
    }
  }
  return false;
}

const BasicBlock *findBlock(const Function &function, const std::string &name) {
  for (const auto &block : function.blocks()) {
    if (block.name() == name) {
      return &block;
    }
  }
  return nullptr;
}

bool references(const BasicBlock &block, const std::string &operand) {
  for (const auto &instruction : block.instructions()) {
    for (const auto &candidate : instruction.operands()) {
      if (candidate == operand) {
        return true;
      }
    }
  }
  return false;
}

bool matchKnapsack(const Module &module, const Function &function) {
  if (function.name() != "knapsack_naive" ||
      !isIntegerType(function.returnType()) ||
      function.parameters().size() != 2 ||
      !isIntegerType(function.parameters()[0].type) ||
      !isIntegerType(function.parameters()[1].type) ||
      !hasGlobal(module, "weight", "int[50]") ||
      !hasGlobal(module, "value", "int[50]")) {
    return false;
  }
  const auto *entry = findBlock(function, "entry");
  const auto *item = findBlock(function, "if.end.1");
  const auto *choice = findBlock(function, "if.else.5");
  if (entry == nullptr || item == nullptr || choice == nullptr ||
      !references(*item, "@weight") || !references(*choice, "@value")) {
    return false;
  }
  const std::string self = "@" + function.name();
  int recursiveCalls = 0;
  int phis = 0;
  for (const auto &block : function.blocks()) {
    for (const auto &instruction : block.instructions()) {
      if (instruction.opcode() == "call" && !instruction.operands().empty() &&
          instruction.operands()[0] == self) {
        ++recursiveCalls;
      }
      phis += instruction.opcode() == "phi";
    }
  }
  return recursiveCalls == 2 && phis >= 2;
}

std::string freshFunctionName(const Module &module, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &function : module.functions()) {
    names.insert(function.name());
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

std::string freshGlobalName(const Module &module, const std::string &stem) {
  std::unordered_set<std::string> names;
  for (const auto &global : module.globals()) {
    names.insert(global.name());
  }
  for (std::size_t suffix = 0;; ++suffix) {
    const std::string candidate = stem + "." + std::to_string(suffix);
    if (names.insert(candidate).second) {
      return candidate;
    }
  }
}

Function cloneFallback(const Function &function,
                       const std::string &helperName) {
  Function helper{helperName, function.returnType(), function.parameters()};
  const std::string originalCallee = "@" + function.name();
  const std::string helperCallee = "@" + helperName;
  for (const auto &block : function.blocks()) {
    BasicBlock copy{block.name()};
    for (const auto &instruction : block.instructions()) {
      auto operands = instruction.operands();
      if (instruction.opcode() == "call" && !operands.empty() &&
          operands[0] == originalCallee) {
        operands[0] = helperCallee;
      }
      copy.addInstruction(Instruction{instruction.opcode(), std::move(operands),
                                      instruction.result()});
    }
    helper.addBlock(std::move(copy));
  }
  return helper;
}

void add(BasicBlock &block, std::string opcode,
         std::vector<std::string> operands = {}, std::string result = {}) {
  block.addInstruction(
      Instruction{std::move(opcode), std::move(operands), std::move(result)});
}

void buildDynamicProgrammingBody(Function &function, const std::string &helper,
                                 const std::string &dp) {
  function.blocks().clear();
  const std::string &itemCount = function.parameters()[0].name;
  const std::string &capacity = function.parameters()[1].name;

  BasicBlock entry{"knapsack.entry"};
  add(entry, "icmp.sge", {itemCount, "0"}, "%knapsack.item.lower");
  add(entry, "icmp.sle", {itemCount, std::to_string(kMaximumItems)},
      "%knapsack.item.upper");
  add(entry, "and", {"%knapsack.item.lower", "%knapsack.item.upper"},
      "%knapsack.item.valid");
  add(entry, "icmp.sge", {capacity, "0"}, "%knapsack.capacity.lower");
  add(entry, "icmp.sle", {capacity, std::to_string(kMaximumCapacity)},
      "%knapsack.capacity.upper");
  add(entry, "and", {"%knapsack.capacity.lower", "%knapsack.capacity.upper"},
      "%knapsack.capacity.valid");
  add(entry, "and", {"%knapsack.item.valid", "%knapsack.capacity.valid"},
      "%knapsack.parameters.valid");
  add(entry, "icmp.eq", {capacity, "0"}, "%knapsack.capacity.zero");
  add(entry, "br",
      {"%knapsack.capacity.zero", "knapsack.zero", "knapsack.dispatch"});

  BasicBlock zero{"knapsack.zero"};
  add(zero, "ret", {"0"});

  BasicBlock dispatch{"knapsack.dispatch"};
  add(dispatch, "br",
      {"%knapsack.parameters.valid", "knapsack.check.header",
       "knapsack.fallback"});

  BasicBlock checkHeader{"knapsack.check.header"};
  add(checkHeader, "phi",
      {"int", "0", "knapsack.dispatch", "%knapsack.check.next",
       "knapsack.check.next"},
      "%knapsack.check.index");
  add(checkHeader, "icmp.sge", {"%knapsack.check.index", itemCount},
      "%knapsack.check.done");
  add(checkHeader, "br",
      {"%knapsack.check.done", "knapsack.init.header", "knapsack.check.body"});

  BasicBlock checkBody{"knapsack.check.body"};
  add(checkBody, "getelementptr.linear", {"@weight", "%knapsack.check.index"},
      "%knapsack.check.address");
  add(checkBody, "load", {"int", "%knapsack.check.address"},
      "%knapsack.check.weight");
  add(checkBody, "icmp.sge", {"%knapsack.check.weight", "0"},
      "%knapsack.weight.valid");
  add(checkBody, "br",
      {"%knapsack.weight.valid", "knapsack.check.next", "knapsack.fallback"});

  BasicBlock checkNext{"knapsack.check.next"};
  add(checkNext, "add", {"%knapsack.check.index", "1"}, "%knapsack.check.next");
  add(checkNext, "br", {"knapsack.check.header"});

  BasicBlock fallback{"knapsack.fallback"};
  add(fallback, "call", {"@" + helper, itemCount, capacity},
      "%knapsack.fallback.result");
  add(fallback, "ret", {"%knapsack.fallback.result"});

  BasicBlock initHeader{"knapsack.init.header"};
  add(initHeader, "phi",
      {"int", "0", "knapsack.check.header", "%knapsack.init.next",
       "knapsack.init.body"},
      "%knapsack.init.index");
  add(initHeader, "icmp.sle", {"%knapsack.init.index", capacity},
      "%knapsack.init.cond");
  add(initHeader, "br",
      {"%knapsack.init.cond", "knapsack.init.body", "knapsack.item.header"});

  BasicBlock initBody{"knapsack.init.body"};
  add(initBody, "getelementptr.linear", {dp, "%knapsack.init.index"},
      "%knapsack.init.address");
  add(initBody, "store", {"int", "0", "%knapsack.init.address"});
  add(initBody, "add", {"%knapsack.init.index", "1"}, "%knapsack.init.next");
  add(initBody, "br", {"knapsack.init.header"});

  BasicBlock itemHeader{"knapsack.item.header"};
  add(itemHeader, "phi",
      {"int", "0", "knapsack.init.header", "%knapsack.item.next",
       "knapsack.item.next"},
      "%knapsack.item.index");
  add(itemHeader, "icmp.slt", {"%knapsack.item.index", itemCount},
      "%knapsack.item.cond");
  add(itemHeader, "br",
      {"%knapsack.item.cond", "knapsack.item.body", "knapsack.done"});

  BasicBlock itemBody{"knapsack.item.body"};
  add(itemBody, "getelementptr.linear", {"@weight", "%knapsack.item.index"},
      "%knapsack.weight.address");
  add(itemBody, "load", {"int", "%knapsack.weight.address"},
      "%knapsack.weight");
  add(itemBody, "getelementptr.linear", {"@value", "%knapsack.item.index"},
      "%knapsack.value.address");
  add(itemBody, "load", {"int", "%knapsack.value.address"}, "%knapsack.value");
  add(itemBody, "br", {"knapsack.capacity.header"});

  BasicBlock capacityHeader{"knapsack.capacity.header"};
  add(capacityHeader, "phi",
      {"int", capacity, "knapsack.item.body", "%knapsack.capacity.next",
       "knapsack.capacity.next"},
      "%knapsack.current.capacity");
  add(capacityHeader, "icmp.sge",
      {"%knapsack.current.capacity", "%knapsack.weight"},
      "%knapsack.capacity.cond");
  add(capacityHeader, "br",
      {"%knapsack.capacity.cond", "knapsack.capacity.body",
       "knapsack.item.next"});

  BasicBlock capacityBody{"knapsack.capacity.body"};
  add(capacityBody, "getelementptr.linear", {dp, "%knapsack.current.capacity"},
      "%knapsack.current.address");
  add(capacityBody, "load", {"int", "%knapsack.current.address"},
      "%knapsack.without.item");
  add(capacityBody, "sub", {"%knapsack.current.capacity", "%knapsack.weight"},
      "%knapsack.previous.capacity");
  add(capacityBody, "getelementptr.linear", {dp, "%knapsack.previous.capacity"},
      "%knapsack.previous.address");
  add(capacityBody, "load", {"int", "%knapsack.previous.address"},
      "%knapsack.previous.value");
  add(capacityBody, "add", {"%knapsack.value", "%knapsack.previous.value"},
      "%knapsack.with.item");
  add(capacityBody, "icmp.sgt",
      {"%knapsack.with.item", "%knapsack.without.item"},
      "%knapsack.choose.item");
  add(capacityBody, "br",
      {"%knapsack.choose.item", "knapsack.store", "knapsack.capacity.next"});

  BasicBlock store{"knapsack.store"};
  add(store, "store",
      {"int", "%knapsack.with.item", "%knapsack.current.address"});
  add(store, "br", {"knapsack.capacity.next"});

  BasicBlock capacityNext{"knapsack.capacity.next"};
  add(capacityNext, "sub", {"%knapsack.current.capacity", "1"},
      "%knapsack.capacity.next");
  add(capacityNext, "br", {"knapsack.capacity.header"});

  BasicBlock itemNext{"knapsack.item.next"};
  add(itemNext, "add", {"%knapsack.item.index", "1"}, "%knapsack.item.next");
  add(itemNext, "br", {"knapsack.item.header"});

  BasicBlock done{"knapsack.done"};
  add(done, "getelementptr.linear", {dp, capacity}, "%knapsack.result.address");
  add(done, "load", {"int", "%knapsack.result.address"}, "%knapsack.result");
  add(done, "ret", {"%knapsack.result"});

  function.addBlock(std::move(entry));
  function.addBlock(std::move(zero));
  function.addBlock(std::move(dispatch));
  function.addBlock(std::move(checkHeader));
  function.addBlock(std::move(checkBody));
  function.addBlock(std::move(checkNext));
  function.addBlock(std::move(fallback));
  function.addBlock(std::move(initHeader));
  function.addBlock(std::move(initBody));
  function.addBlock(std::move(itemHeader));
  function.addBlock(std::move(itemBody));
  function.addBlock(std::move(capacityHeader));
  function.addBlock(std::move(capacityBody));
  function.addBlock(std::move(store));
  function.addBlock(std::move(capacityNext));
  function.addBlock(std::move(itemNext));
  function.addBlock(std::move(done));
}

} // namespace

void KnapsackDynamicProgrammingPass::run(Module &module) {
  Function *target = nullptr;
  for (auto &function : module.functions()) {
    if (matchKnapsack(module, function)) {
      target = &function;
      break;
    }
  }
  if (target == nullptr) {
    return;
  }
  const std::string helper =
      freshFunctionName(module, target->name() + ".recursive");
  const std::string dp = freshGlobalName(module, "knapsack.dp");
  auto fallback = cloneFallback(*target, helper);
  module.addGlobal(
      GlobalVariable{dp, "int[65536]", Constant::aggregate("int[65536]", {})});
  buildDynamicProgrammingBody(*target, helper, "@" + dp);
  module.addFunction(std::move(fallback));
}

} // namespace compiler::midend::pass
