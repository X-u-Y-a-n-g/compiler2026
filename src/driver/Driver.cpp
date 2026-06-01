#include "../../include/compiler/driver/Driver.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "../../include/compiler/backend/frame/FrameLowering.h"
#include "../../include/compiler/backend/isel/InstructionSelector.h"
#include "../../include/compiler/backend/opt/StackObjectPromotion.h"
#include "../../include/compiler/backend/regalloc/RegisterAllocator.h"
#include "../../include/compiler/backend/stack/StackLowering.h"
#include "../../include/compiler/backend/target/TargetBackend.h"
#include "../../include/compiler/backend/target/TargetInfo.h"
#include "../../include/compiler/driver/CompilerInvocation.h"
#include "../../include/compiler/driver/Pipeline.h"
#include "../../include/compiler/frontend/ast/AstPrinter.h"
#include "../../include/compiler/frontend/lexer/Lexer.h"
#include "../../include/compiler/frontend/lexer/Token.h"
#include "../../include/compiler/frontend/parser/Parser.h"
#include "../../include/compiler/frontend/sema/SemanticAnalyzer.h"
#include "../../include/compiler/frontend/types/TypeChecker.h"
#include "../../include/compiler/ir/builder/AstToIR.h"
#include "../../include/compiler/ir/printer/IRPrinter.h"
#include "../../include/compiler/ir/verify/IRVerifier.h"
#include "../../include/compiler/midend/pass/AlgebraicSimplificationPass.h"
#include "../../include/compiler/midend/pass/BranchSimplificationPass.h"
#include "../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"
#include "../../include/compiler/midend/pass/DeadCodeEliminationPass.h"
#include "../../include/compiler/midend/pass/FunctionInliningPass.h"
#include "../../include/compiler/midend/pass/Mem2RegPass.h"
#include "../../include/compiler/midend/pass/PassManager.h"
#include "../../include/compiler/midend/pass/RepeatedAccumulationEliminationPass.h"
#include "../../include/compiler/midend/pass/SparseConditionalConstantPropagationPass.h"
#include "../../include/compiler/midend/pass/TrivialBlockEliminationPass.h"
#include "../../include/compiler/support/FileSystem.h"
#include "../../include/compiler/midend/pass/LoopInvariantCodeMotionPass.h"

namespace compiler::driver {

namespace {

void printUsage(std::ostream& os) {
  os << "usage: compiler <input> [-o output] [-S] [-O0|-O1] [--target riscv64]\n";
  os << "       compiler <input> [--dump-tokens] [--dump-ast] [--dump-ir]\n";
}

}  // namespace

CompilerInvocation CompilerInvocation::fromCommandLine(
    int argc, const char* const* argv, compiler::support::DiagnosticEngine& diagnostics) {
  CompilerInvocation invocation;
  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-S") {
      options.emitAssembly = true;
      continue;
    }
    if (arg == "--dump-tokens") {
      options.dumpTokens = true;
      continue;
    }
    if (arg == "--dump-ast") {
      options.dumpAst = true;
      continue;
    }
    if (arg == "--dump-ir") {
      options.dumpIr = true;
      continue;
    }
    if (arg == "-O0") {
      options.optLevel = 0;
      continue;
    }
    if (arg == "-O1") {
      options.optLevel = 1;
      continue;
    }
    if (arg == "-o") {
      if (i + 1 >= argc) {
        diagnostics.error({}, "missing output path after -o");
        break;
      }
      options.outputPath = argv[++i];
      continue;
    }
    if (arg == "--target") {
      if (i + 1 >= argc) {
        diagnostics.error({}, "missing target name after --target");
        break;
      }
      options.target = argv[++i];
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      diagnostics.error({}, "unknown option: " + arg);
      continue;
    }
    if (!options.inputPath.empty()) {
      diagnostics.error({}, "multiple input files are not supported yet");
      continue;
    }
    options.inputPath = arg;
  }

  if (options.inputPath.empty()) {
    diagnostics.error({}, "missing input file");
  }

  invocation.options_ = std::move(options);
  invocation.valid_ = !diagnostics.hasErrors();
  return invocation;
}

const Options& CompilerInvocation::options() const {
  return options_;
}

bool CompilerInvocation::valid() const {
  return valid_;
}

int Pipeline::execute(const CompilerInvocation& invocation,
                      compiler::support::DiagnosticEngine& diagnostics) const {
  std::string source;
  if (!compiler::support::FileSystem::readTextFile(invocation.options().inputPath, source,
                                                   diagnostics)) {
    return 1;
  }

  compiler::frontend::lexer::Lexer lexer(source);
  auto tokens = lexer.lex(diagnostics);
  if (invocation.options().dumpTokens) {
    std::cout << compiler::frontend::lexer::dumpTokens(tokens);
  }
  if (diagnostics.hasErrors()) {
    return 1;
  }

  compiler::frontend::parser::Parser parser(tokens);
  auto ast = parser.parse(diagnostics);
  if (!ast || diagnostics.hasErrors()) {
    return 1;
  }

  if (invocation.options().dumpAst) {
    std::cout << compiler::frontend::ast::AstPrinter{}.print(*ast);
  }

  if (!compiler::frontend::sema::SemanticAnalyzer{}.analyze(*ast, diagnostics)) {
    return 1;
  }
  if (!compiler::frontend::types::TypeChecker{}.check(*ast, diagnostics)) {
    return 1;
  }

  auto module = compiler::ir::builder::AstToIR{}.lower(*ast, invocation.options().inputPath);

  compiler::midend::pass::PassManager passManager;
  if (invocation.options().optLevel > 0) {
    passManager.add(std::make_unique<compiler::midend::pass::AlgebraicSimplificationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::Mem2RegPass>());
    //passManager.add(std::make_unique<compiler::midend::pass::AlgebraicSimplificationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::FunctionInliningPass>());
   // passManager.add(std::make_unique<compiler::midend::pass::AlgebraicSimplificationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::SparseConditionalConstantPropagationPass>());
   // passManager.add(std::make_unique<compiler::midend::pass::BranchSimplificationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::LoopInvariantCodeMotionPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::RepeatedAccumulationEliminationPass>());
  //  passManager.add(std::make_unique<compiler::midend::pass::BranchSimplificationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::DeadCodeEliminationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::TrivialBlockEliminationPass>());
  }
  passManager.run(module);

  const auto irErrors = compiler::ir::verify::IRVerifier{}.verify(module);
  for (const auto& error : irErrors) {
    diagnostics.error({}, "IR verifier: " + error);
  }
  if (!irErrors.empty()) {
    return 1;
  }

  if (invocation.options().dumpIr) {
    std::cout << compiler::ir::printer::IRPrinter{}.print(module);
  }
  if (invocation.options().dumpIr && invocation.options().outputPath.empty()) {
    return diagnostics.hasErrors() ? 1 : 0;
  }

  const auto target = compiler::backend::target::TargetInfo::fromName(invocation.options().target);
  const auto assembly = compiler::backend::target::backendFor(target).generateAssembly(module);

  if (invocation.options().outputPath.empty()) {
    std::cout << assembly;
    return diagnostics.hasErrors() ? 1 : 0;
  }

  if (!compiler::support::FileSystem::writeTextFile(invocation.options().outputPath, assembly,
                                                    diagnostics)) {
    return 1;
  }
  return diagnostics.hasErrors() ? 1 : 0;
}

int Driver::run(int argc, const char* const* argv) const {
  if (argc <= 1) {
    printUsage(std::cerr);
    return 1;
  }
  if (argc == 2 && std::string(argv[1]) == "--help") {
    printUsage(std::cout);
    return 0;
  }

  compiler::support::DiagnosticEngine diagnostics;
  const auto invocation = CompilerInvocation::fromCommandLine(argc, argv, diagnostics);
  if (!invocation.valid()) {
    diagnostics.print(std::cerr);
    printUsage(std::cerr);
    return 1;
  }

  const int exitCode = Pipeline{}.execute(invocation, diagnostics);
  if (!diagnostics.diagnostics().empty()) {
    diagnostics.print(std::cerr);
  }
  return exitCode;
}

}  // namespace compiler::driver
