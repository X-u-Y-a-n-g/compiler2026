#include "compiler/driver/Driver.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "compiler/backend/asm/AsmPrinter.h"
#include "compiler/backend/frame/FrameLowering.h"
#include "compiler/backend/isel/InstructionSelector.h"
#include "compiler/backend/regalloc/RegisterAllocator.h"
#include "compiler/backend/target/TargetInfo.h"
#include "compiler/driver/CompilerInvocation.h"
#include "compiler/driver/Pipeline.h"
#include "compiler/frontend/ast/AstPrinter.h"
#include "compiler/frontend/lexer/Lexer.h"
#include "compiler/frontend/lexer/Token.h"
#include "compiler/frontend/parser/Parser.h"
#include "compiler/frontend/sema/SemanticAnalyzer.h"
#include "compiler/frontend/types/TypeChecker.h"
#include "compiler/ir/builder/AstToIR.h"
#include "compiler/ir/printer/IRPrinter.h"
#include "compiler/ir/verify/IRVerifier.h"
#include "compiler/midend/pass/PassManager.h"
#include "compiler/support/FileSystem.h"

namespace compiler::driver {

namespace {

void printUsage(std::ostream& os) {
  os << "usage: compiler <input> [-o output] [-S] [-O0|-O1] [--target riscv64|armv7]\n";
  os << "       compiler <input> [--dump-tokens] [--dump-ast] [--dump-ir]\n";
}

class PipelineMarkerPass final : public compiler::midend::pass::ModulePass {
 public:
  std::string name() const override { return "pipeline-marker"; }
  void run(compiler::ir::core::Module&) override {}
};

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
    passManager.add(std::make_unique<PipelineMarkerPass>());
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
  auto machineModule = compiler::backend::isel::InstructionSelector{}.select(module, target);
  compiler::backend::regalloc::RegisterAllocator{}.allocate(machineModule);
  compiler::backend::frame::FrameLowering{}.lower(machineModule);
  const auto assembly = compiler::backend::asmcode::AsmPrinter{}.print(machineModule);

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
