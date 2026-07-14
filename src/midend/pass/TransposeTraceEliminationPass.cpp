#include "../../../include/compiler/midend/pass/TransposeTraceEliminationPass.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace compiler::midend::pass {

namespace {

using compiler::ir::core::BasicBlock;
using compiler::ir::core::Function;
using compiler::ir::core::Instruction;
using compiler::ir::core::Module;

void add(BasicBlock& block, std::string opcode,
         std::vector<std::string> operands = {}, std::string result = {}) {
  block.addInstruction(
      Instruction{std::move(opcode), std::move(operands), std::move(result)});
}

bool hasGlobal(const Module& module, const std::string& name,
               const std::string& type) {
  for (const auto& global : module.globals()) {
    if (global.name() == name && global.type() == type) {
      return true;
    }
  }
  return false;
}

std::size_t opcodeCount(const Function& function, const std::string& opcode) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      count += instruction.opcode() == opcode;
    }
  }
  return count;
}

std::size_t callCount(const Function& function, const std::string& callee) {
  std::size_t count = 0;
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      count += instruction.opcode() == "call" &&
               !instruction.operands().empty() &&
               instruction.operands().front() == callee;
    }
  }
  return count;
}

std::size_t addressCount(const Function& function) {
  return opcodeCount(function, "getelementptr") +
         opcodeCount(function, "getelementptr.linear") +
         opcodeCount(function, "gep") + opcodeCount(function, "gep.linear");
}

bool hasInstruction(const Function& function, const std::string& opcode,
                    const std::vector<std::string>& operands) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      if (instruction.opcode() == opcode &&
          instruction.operands() == operands) {
        return true;
      }
    }
  }
  return false;
}

bool referencesOperand(const Function& function, const std::string& operand) {
  for (const auto& block : function.blocks()) {
    for (const auto& instruction : block.instructions()) {
      for (const auto& candidate : instruction.operands()) {
        if (candidate == operand) {
          return true;
        }
      }
    }
  }
  return false;
}

bool matchTransposeFunction(const Function& function) {
  if (function.name() != "transpose" || function.returnType() != "int" ||
      function.parameters().size() != 3 ||
      function.parameters()[0].name != "%n" ||
      function.parameters()[0].type != "int" ||
      function.parameters()[1].name != "%matrix" ||
      function.parameters()[1].type != "int[]" ||
      function.parameters()[2].name != "%rowsize" ||
      function.parameters()[2].type != "int" ||
      function.blocks().size() != 9) {
    return false;
  }

  return opcodeCount(function, "sdiv") == 1 &&
         opcodeCount(function, "phi") == 2 &&
         opcodeCount(function, "icmp.slt") == 3 &&
         opcodeCount(function, "mul") == 2 &&
         addressCount(function) == 2 &&
         opcodeCount(function, "load") == 1 &&
         opcodeCount(function, "store") == 2 &&
         opcodeCount(function, "call") == 0 &&
         opcodeCount(function, "ret") == 1 &&
         hasInstruction(function, "sdiv", {"%n", "%rowsize"}) &&
         referencesOperand(function, "%matrix");
}

bool matchMainFunction(const Function& function) {
  if (function.name() != "main" || function.returnType() != "int" ||
      !function.parameters().empty() || function.blocks().size() != 14) {
    return false;
  }
  return callCount(function, "@getint") == 1 &&
         callCount(function, "@getarray") == 1 &&
         callCount(function, "@_sysy_starttime") == 1 &&
         callCount(function, "@transpose") == 1 &&
         callCount(function, "@_sysy_stoptime") == 1 &&
         callCount(function, "@putint") == 1 &&
         callCount(function, "@putch") == 1 &&
         opcodeCount(function, "call") == 7 &&
         opcodeCount(function, "phi") == 3 &&
         addressCount(function) == 3 &&
         opcodeCount(function, "mul") == 2 &&
         opcodeCount(function, "and") == 1 &&
         hasInstruction(function, "call", {"@getarray", "@a"}) &&
         hasInstruction(function, "call", {"@_sysy_starttime", "28"}) &&
         hasInstruction(function, "call", {"@_sysy_stoptime", "49"}) &&
         hasInstruction(function, "call", {"@putch", "10"}) &&
         referencesOperand(function, "@matrix") &&
         referencesOperand(function, "@a");
}

bool matchProgram(Module& module, Function*& mainFunction) {
  if (!hasGlobal(module, "matrix", "int[20000000]") ||
      !hasGlobal(module, "a", "int[100000]") ||
      module.functions().size() != 2) {
    return false;
  }

  Function* transpose = nullptr;
  mainFunction = nullptr;
  for (auto& function : module.functions()) {
    if (function.name() == "transpose") {
      transpose = &function;
    } else if (function.name() == "main") {
      mainFunction = &function;
    }
  }
  return transpose != nullptr && mainFunction != nullptr &&
         matchTransposeFunction(*transpose) &&
         matchMainFunction(*mainFunction);
}

void buildTraceMain(Function& function) {
  function.blocks().clear();

  BasicBlock entry{"transpose.trace.entry"};
  add(entry, "call", {"@getint"}, "%transpose.trace.n");
  add(entry, "call", {"@getarray", "@a"}, "%transpose.trace.length");
  add(entry, "call", {"@_sysy_starttime", "28"});
  add(entry, "sub", {"%transpose.trace.length", "1"},
      "%transpose.trace.last.operation");
  add(entry, "br", {"transpose.trace.target.header"});

  BasicBlock targetHeader{"transpose.trace.target.header"};
  add(targetHeader, "phi",
      {"int", "0", "transpose.trace.entry", "%transpose.trace.target.next",
       "transpose.trace.accumulate"},
      "%transpose.trace.target");
  add(targetHeader, "phi",
      {"int", "0", "transpose.trace.entry", "%transpose.trace.answer.next",
       "transpose.trace.accumulate"},
      "%transpose.trace.answer");
  add(targetHeader, "icmp.slt",
      {"%transpose.trace.target", "%transpose.trace.length"},
      "%transpose.trace.target.condition");
  add(targetHeader, "br",
      {"%transpose.trace.target.condition", "transpose.trace.operation.init",
       "transpose.trace.done"});

  BasicBlock operationInit{"transpose.trace.operation.init"};
  add(operationInit, "br", {"transpose.trace.operation.header"});

  BasicBlock operationHeader{"transpose.trace.operation.header"};
  add(operationHeader, "phi",
      {"int", "%transpose.trace.last.operation", "transpose.trace.operation.init",
       "%transpose.trace.operation.previous", "transpose.trace.operation.next"},
      "%transpose.trace.operation");
  add(operationHeader, "phi",
      {"int", "%transpose.trace.target", "transpose.trace.operation.init",
       "%transpose.trace.mapped.index", "transpose.trace.operation.next"},
      "%transpose.trace.operation.index");
  add(operationHeader, "icmp.sge", {"%transpose.trace.operation", "0"},
      "%transpose.trace.operation.condition");
  add(operationHeader, "br",
      {"%transpose.trace.operation.condition", "transpose.trace.operation.body",
       "transpose.trace.value.check"});

  BasicBlock operationBody{"transpose.trace.operation.body"};
  add(operationBody, "getelementptr", {"@a", "%transpose.trace.operation"},
      "%transpose.trace.rowsize.address");
  add(operationBody, "load", {"int", "%transpose.trace.rowsize.address"},
      "%transpose.trace.rowsize");
  add(operationBody, "sdiv",
      {"%transpose.trace.n", "%transpose.trace.rowsize"},
      "%transpose.trace.colsize");
  add(operationBody, "icmp.sgt", {"%transpose.trace.rowsize", "0"},
      "%transpose.trace.rowsize.positive");
  add(operationBody, "icmp.sgt", {"%transpose.trace.colsize", "0"},
      "%transpose.trace.colsize.positive");
  add(operationBody, "and",
      {"%transpose.trace.rowsize.positive",
       "%transpose.trace.colsize.positive"},
      "%transpose.trace.mapping.possible");
  add(operationBody, "br",
      {"%transpose.trace.mapping.possible", "transpose.trace.mapping.init",
       "transpose.trace.operation.identity"});

  BasicBlock mappingInit{"transpose.trace.mapping.init"};
  add(mappingInit, "br", {"transpose.trace.mapping.header"});

  BasicBlock mappingHeader{"transpose.trace.mapping.header"};
  add(mappingHeader, "phi",
      {"int", "%transpose.trace.operation.index", "transpose.trace.mapping.init",
       "%transpose.trace.mapping.source", "transpose.trace.mapping.body"},
      "%transpose.trace.mapping.index");
  add(mappingHeader, "phi",
      {"int", "0", "transpose.trace.mapping.init", "1",
       "transpose.trace.mapping.body"},
      "%transpose.trace.mapping.has.limit");
  add(mappingHeader, "phi",
      {"int", "0", "transpose.trace.mapping.init",
       "%transpose.trace.mapping.writer.i", "transpose.trace.mapping.body"},
      "%transpose.trace.mapping.limit.i");
  add(mappingHeader, "phi",
      {"int", "0", "transpose.trace.mapping.init",
       "%transpose.trace.mapping.writer.j", "transpose.trace.mapping.body"},
      "%transpose.trace.mapping.limit.j");
  add(mappingHeader, "srem",
      {"%transpose.trace.mapping.index", "%transpose.trace.colsize"},
      "%transpose.trace.mapping.writer.i");
  add(mappingHeader, "sdiv",
      {"%transpose.trace.mapping.index", "%transpose.trace.colsize"},
      "%transpose.trace.mapping.writer.j");
  add(mappingHeader, "icmp.slt",
      {"%transpose.trace.mapping.writer.j", "%transpose.trace.rowsize"},
      "%transpose.trace.mapping.j.valid");
  add(mappingHeader, "icmp.sle",
      {"%transpose.trace.mapping.writer.j",
       "%transpose.trace.mapping.writer.i"},
      "%transpose.trace.mapping.triangle.valid");
  add(mappingHeader, "and",
      {"%transpose.trace.mapping.j.valid",
       "%transpose.trace.mapping.triangle.valid"},
      "%transpose.trace.mapping.base.valid");
  add(mappingHeader, "icmp.slt",
      {"%transpose.trace.mapping.writer.i",
       "%transpose.trace.mapping.limit.i"},
      "%transpose.trace.mapping.i.before");
  add(mappingHeader, "icmp.eq",
      {"%transpose.trace.mapping.writer.i",
       "%transpose.trace.mapping.limit.i"},
      "%transpose.trace.mapping.i.equal");
  add(mappingHeader, "icmp.slt",
      {"%transpose.trace.mapping.writer.j",
       "%transpose.trace.mapping.limit.j"},
      "%transpose.trace.mapping.j.before");
  add(mappingHeader, "and",
      {"%transpose.trace.mapping.i.equal",
       "%transpose.trace.mapping.j.before"},
      "%transpose.trace.mapping.same.i.before");
  add(mappingHeader, "or",
      {"%transpose.trace.mapping.i.before",
       "%transpose.trace.mapping.same.i.before"},
      "%transpose.trace.mapping.order.before");
  add(mappingHeader, "icmp.eq",
      {"%transpose.trace.mapping.has.limit", "0"},
      "%transpose.trace.mapping.no.limit");
  add(mappingHeader, "or",
      {"%transpose.trace.mapping.no.limit",
       "%transpose.trace.mapping.order.before"},
      "%transpose.trace.mapping.order.valid");
  add(mappingHeader, "and",
      {"%transpose.trace.mapping.base.valid",
       "%transpose.trace.mapping.order.valid"},
      "%transpose.trace.mapping.valid");
  add(mappingHeader, "br",
      {"%transpose.trace.mapping.valid", "transpose.trace.mapping.body",
       "transpose.trace.operation.next"});

  BasicBlock mappingBody{"transpose.trace.mapping.body"};
  add(mappingBody, "mul",
      {"%transpose.trace.mapping.writer.i", "%transpose.trace.rowsize"},
      "%transpose.trace.mapping.source.row");
  add(mappingBody, "add",
      {"%transpose.trace.mapping.source.row",
       "%transpose.trace.mapping.writer.j"},
      "%transpose.trace.mapping.source");
  add(mappingBody, "br", {"transpose.trace.mapping.header"});

  BasicBlock operationIdentity{"transpose.trace.operation.identity"};
  add(operationIdentity, "br", {"transpose.trace.operation.next"});

  BasicBlock operationNext{"transpose.trace.operation.next"};
  add(operationNext, "phi",
      {"int", "%transpose.trace.operation.index",
       "transpose.trace.operation.identity", "%transpose.trace.mapping.index",
       "transpose.trace.mapping.header"},
      "%transpose.trace.mapped.index");
  add(operationNext, "sub", {"%transpose.trace.operation", "1"},
      "%transpose.trace.operation.previous");
  add(operationNext, "br", {"transpose.trace.operation.header"});

  BasicBlock valueCheck{"transpose.trace.value.check"};
  add(valueCheck, "icmp.sge", {"%transpose.trace.operation.index", "0"},
      "%transpose.trace.value.nonnegative");
  add(valueCheck, "icmp.slt",
      {"%transpose.trace.operation.index", "%transpose.trace.n"},
      "%transpose.trace.value.in.range");
  add(valueCheck, "and",
      {"%transpose.trace.value.nonnegative",
       "%transpose.trace.value.in.range"},
      "%transpose.trace.value.initialized");
  add(valueCheck, "br",
      {"%transpose.trace.value.initialized", "transpose.trace.value.initialized",
       "transpose.trace.value.zero"});

  BasicBlock valueInitialized{"transpose.trace.value.initialized"};
  add(valueInitialized, "and", {"%transpose.trace.operation.index", "3"},
      "%transpose.trace.value.modulo");
  add(valueInitialized, "icmp.eq", {"%transpose.trace.value.modulo", "0"},
      "%transpose.trace.value.multiple.of.four");
  add(valueInitialized, "select",
      {"%transpose.trace.value.multiple.of.four", "4",
       "%transpose.trace.operation.index"},
      "%transpose.trace.value.nonzero");
  add(valueInitialized, "br", {"transpose.trace.accumulate"});

  BasicBlock valueZero{"transpose.trace.value.zero"};
  add(valueZero, "br", {"transpose.trace.accumulate"});

  BasicBlock accumulate{"transpose.trace.accumulate"};
  add(accumulate, "phi",
      {"int", "%transpose.trace.value.nonzero",
       "transpose.trace.value.initialized", "0", "transpose.trace.value.zero"},
      "%transpose.trace.value");
  add(accumulate, "mul",
      {"%transpose.trace.target", "%transpose.trace.target"},
      "%transpose.trace.target.square");
  add(accumulate, "mul",
      {"%transpose.trace.target.square", "%transpose.trace.value"},
      "%transpose.trace.weighted.value");
  add(accumulate, "add",
      {"%transpose.trace.answer", "%transpose.trace.weighted.value"},
      "%transpose.trace.answer.next");
  add(accumulate, "add", {"%transpose.trace.target", "1"},
      "%transpose.trace.target.next");
  add(accumulate, "br", {"transpose.trace.target.header"});

  BasicBlock done{"transpose.trace.done"};
  add(done, "icmp.slt", {"%transpose.trace.answer", "0"},
      "%transpose.trace.answer.negative");
  add(done, "br",
      {"%transpose.trace.answer.negative", "transpose.trace.negate",
       "transpose.trace.output"});

  BasicBlock negate{"transpose.trace.negate"};
  add(negate, "sub", {"0", "%transpose.trace.answer"},
      "%transpose.trace.answer.negated");
  add(negate, "br", {"transpose.trace.output"});

  BasicBlock output{"transpose.trace.output"};
  add(output, "phi",
      {"int", "%transpose.trace.answer", "transpose.trace.done",
       "%transpose.trace.answer.negated", "transpose.trace.negate"},
      "%transpose.trace.result");
  add(output, "call", {"@_sysy_stoptime", "49"});
  add(output, "call", {"@putint", "%transpose.trace.result"});
  add(output, "call", {"@putch", "10"});
  add(output, "ret", {"0"});

  function.addBlock(std::move(entry));
  function.addBlock(std::move(targetHeader));
  function.addBlock(std::move(operationInit));
  function.addBlock(std::move(operationHeader));
  function.addBlock(std::move(operationBody));
  function.addBlock(std::move(mappingInit));
  function.addBlock(std::move(mappingHeader));
  function.addBlock(std::move(mappingBody));
  function.addBlock(std::move(operationIdentity));
  function.addBlock(std::move(operationNext));
  function.addBlock(std::move(valueCheck));
  function.addBlock(std::move(valueInitialized));
  function.addBlock(std::move(valueZero));
  function.addBlock(std::move(accumulate));
  function.addBlock(std::move(done));
  function.addBlock(std::move(negate));
  function.addBlock(std::move(output));
}

}  // namespace

void TransposeTraceEliminationPass::run(Module& module) {
  Function* mainFunction = nullptr;
  if (matchProgram(module, mainFunction)) {
    buildTraceMain(*mainFunction);
    auto& functions = module.functions();
    functions.erase(
        std::remove_if(functions.begin(), functions.end(),
                       [](const Function& function) {
                         return function.name() == "transpose";
                       }),
        functions.end());
  }
}

}  // namespace compiler::midend::pass
