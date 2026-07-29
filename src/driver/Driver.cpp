#include "../../include/compiler/driver/Driver.h"

#include <cstdlib>
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
#include "../../include/compiler/midend/pass/AffineArrayCopyPropagationPass.h"
#include "../../include/compiler/midend/pass/AdjacentProducerConsumerLoopFusionPass.h"
#include "../../include/compiler/midend/pass/ArraySummaryDimensionCollapsePass.h"
#include "../../include/compiler/midend/pass/AffineModularRecurrencePass.h"
#include "../../include/compiler/midend/pass/AArch64IntegerDotReductionPass.h"
#include "../../include/compiler/midend/pass/AArch64PureMapRecurrenceVectorizationPass.h"
#include "../../include/compiler/midend/pass/AArch64VectorConstantRemainderPass.h"
#include "../../include/compiler/midend/pass/BitwiseIdiomRecognitionPass.h"
#include "../../include/compiler/midend/pass/BoundedModuloReductionPass.h"
#include "../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"
#include "../../include/compiler/midend/pass/ConstantArgumentPropagationPass.h"
#include "../../include/compiler/midend/pass/ConstantFoldingPass.h"
#include "../../include/compiler/midend/pass/CrossLoopStoreForwardingPass.h"
#include "../../include/compiler/midend/pass/DeadCodeEliminationPass.h"
#include "../../include/compiler/midend/pass/DivRemPairStrengthReductionPass.h"
#include "../../include/compiler/midend/pass/FunctionInliningPass.h"
#include "../../include/compiler/midend/pass/GlobalScalarLocalizationPass.h"
#include "../../include/compiler/midend/pass/GuardedInitializationSinkingPass.h"
#include "../../include/compiler/midend/pass/LoopInvariantCodeMotionPass.h"
#include "../../include/compiler/midend/pass/LoopCarriedStoreToLoadForwardingPass.h"
#include "../../include/compiler/midend/pass/LoopInterchangeRowUpdatePass.h"
#include "../../include/compiler/midend/pass/LoopWavefrontColumnPackingPass.h"
#include "../../include/compiler/midend/pass/LoopScalarPromotionPass.h"
#include "../../include/compiler/midend/pass/LoopUnrollAndJamPass.h"
#include "../../include/compiler/midend/pass/Mem2RegPass.h"
#include "../../include/compiler/midend/pass/MonotonicContinueEliminationPass.h"
#include "../../include/compiler/midend/pass/MonotonicLoopBoundPass.h"
#include "../../include/compiler/midend/pass/NestedInvariantReductionPass.h"
#include "../../include/compiler/midend/pass/PassManager.h"
#include "../../include/compiler/midend/pass/PowerOfTwoDispatchPass.h"
#include "../../include/compiler/midend/pass/RedundantLoadEliminationPass.h"
#include "../../include/compiler/midend/pass/RedundantStoreEliminationPass.h"
#include "../../include/compiler/midend/pass/RecursiveInliningPass.h"
#include "../../include/compiler/midend/pass/RemainderComparisonPass.h"
#include "../../include/compiler/midend/pass/StoreProducerReductionFusionPass.h"
#include "../../include/compiler/midend/pass/StackAggregateInitPruningPass.h"
#include "../../include/compiler/midend/pass/TailRecursionEliminationPass.h"
#include "../../include/compiler/midend/pass/TrivialBlockEliminationPass.h"
#include "../../include/compiler/midend/pass/WidenedModularMultiplyPass.h"
#include "../../include/compiler/support/FileSystem.h"

namespace compiler::driver {

namespace {

void printUsage(std::ostream& os) {
  os << "usage: compiler <input> [-o output] [-S] [-O0|-O1] [--target riscv64|aarch64]\n";
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
    passManager.add(std::make_unique<compiler::midend::pass::Mem2RegPass>());
    passManager.add(std::make_unique<compiler::midend::pass::ConstantFoldingPass>());
    passManager.add(std::make_unique<compiler::midend::pass::FunctionInliningPass>());
    passManager.add(std::make_unique<
                    compiler::midend::pass::ConstantArgumentPropagationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::TailRecursionEliminationPass>());
    if (invocation.options().target == "aarch64" ||
        invocation.options().target == "arm64") {
      passManager.add(
          std::make_unique<compiler::midend::pass::WidenedModularMultiplyPass>());
    }
    passManager.add(
        std::make_unique<compiler::midend::pass::RecursiveInliningPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::GlobalScalarLocalizationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::Mem2RegPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::BitwiseIdiomRecognitionPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::PowerOfTwoDispatchPass>());
    if (std::getenv("COMPILER_DISABLE_LATE_HELPER_INLINE_CACHE") == nullptr &&
        std::getenv("COMPILER_DISABLE_CLOSED_HELPER_STATE_PIPELINE") ==
            nullptr) {
      passManager.add(
          std::make_unique<compiler::midend::pass::FunctionInliningPass>());
      if (std::getenv("COMPILER_DISABLE_AGGRESSIVE_STATE_CHAIN_PIPELINE") ==
          nullptr) {
        passManager.add(
            std::make_unique<compiler::midend::pass::ConstantFoldingPass>(true));
      }
      passManager.add(std::make_unique<
                      compiler::midend::pass::GlobalScalarLocalizationPass>());
      passManager.add(std::make_unique<compiler::midend::pass::Mem2RegPass>());
      if (std::getenv("COMPILER_DISABLE_AGGRESSIVE_STATE_CHAIN_PIPELINE") ==
          nullptr) {
        passManager.add(
            std::make_unique<compiler::midend::pass::ConstantFoldingPass>(true));
        passManager.add(std::make_unique<
                        compiler::midend::pass::CommonSubexpressionEliminationPass>());
        passManager.add(
            std::make_unique<compiler::midend::pass::DeadCodeEliminationPass>());
      }
    }
    passManager.add(
        std::make_unique<compiler::midend::pass::AffineModularRecurrencePass>());
    if (invocation.options().target == "aarch64" ||
        invocation.options().target == "arm64") {
      passManager.add(std::make_unique<
                      compiler::midend::pass::ArraySummaryDimensionCollapsePass>());
    }
    passManager.add(
        std::make_unique<compiler::midend::pass::AffineArrayCopyPropagationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    passManager.add(std::make_unique<compiler::midend::pass::LoopInvariantCodeMotionPass>());
    const bool isAArch64 = invocation.options().target == "aarch64" ||
                           invocation.options().target == "arm64";
    if (isAArch64) {
      passManager.add(std::make_unique<
                      compiler::midend::pass::
                          AdjacentProducerConsumerLoopFusionPass>());
    }
    // Thirty-two lanes map exactly to eight i32x4 NEON accumulators.  RV64 keeps
    // the empirically pressure-safe ten-lane scalar factor.
    passManager.add(std::make_unique<compiler::midend::pass::LoopUnrollAndJamPass>(
        isAArch64 ? 32 : 10, isAArch64));
    passManager.add(
        std::make_unique<compiler::midend::pass::CrossLoopStoreForwardingPass>());
    passManager.add(std::make_unique<compiler::midend::pass::Mem2RegPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::DivRemPairStrengthReductionPass>());
    passManager.add(std::make_unique<compiler::midend::pass::RemainderComparisonPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::RedundantLoadEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::RedundantStoreEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::RedundantLoadEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::RedundantStoreEliminationPass>());
    if (std::getenv("COMPILER_DISABLE_OBJECT_SENSITIVE_LOAD_FORWARDING") ==
        nullptr) {
      passManager.add(
          std::make_unique<compiler::midend::pass::CommonSubexpressionEliminationPass>());
    }
    if (invocation.options().target == "aarch64" ||
        invocation.options().target == "arm64") {
      passManager.add(
          std::make_unique<compiler::midend::pass::BoundedModuloReductionPass>());
      passManager.add(std::make_unique<
                      compiler::midend::pass::LoopWavefrontColumnPackingPass>());
      passManager.add(std::make_unique<
                      compiler::midend::pass::LoopInterchangeRowUpdatePass>());
      passManager.add(std::make_unique<
                      compiler::midend::pass::AArch64IntegerDotReductionPass>());
      passManager.add(std::make_unique<
                      compiler::midend::pass::ConstantFoldingPass>());
      passManager.add(std::make_unique<
                      compiler::midend::pass::CommonSubexpressionEliminationPass>());
    }
    passManager.add(
        std::make_unique<compiler::midend::pass::MonotonicContinueEliminationPass>());
    passManager.add(
        std::make_unique<compiler::midend::pass::MonotonicLoopBoundPass>());
    if ((invocation.options().target == "aarch64" ||
         invocation.options().target == "arm64") &&
        std::getenv(
            "COMPILER_DISABLE_LOOP_CARRIED_STORE_LOAD_FORWARDING") ==
            nullptr) {
      passManager.add(std::make_unique<compiler::midend::pass::
                                          LoopCarriedStoreToLoadForwardingPass>());
    }
    passManager.add(
        std::make_unique<compiler::midend::pass::LoopScalarPromotionPass>());
    if (invocation.options().target == "aarch64" ||
        invocation.options().target == "arm64") {
      passManager.add(std::make_unique<
                      compiler::midend::pass::AArch64PureMapRecurrenceVectorizationPass>());
      passManager.add(std::make_unique<
                      compiler::midend::pass::AArch64VectorConstantRemainderPass>());
    }
    passManager.add(
        std::make_unique<compiler::midend::pass::NestedInvariantReductionPass>());
    if (invocation.options().target == "aarch64" ||
        invocation.options().target == "arm64") {
      passManager.add(std::make_unique<
                      compiler::midend::pass::StoreProducerReductionFusionPass>());
    }
    if (std::getenv("COMPILER_DISABLE_STACK_AGGREGATE_INIT_PRUNING") ==
        nullptr) {
      passManager.add(std::make_unique<
                      compiler::midend::pass::StackAggregateInitPruningPass>());
    }
    if (std::getenv("COMPILER_DISABLE_GUARDED_INITIALIZATION_SINKING") ==
        nullptr) {
      passManager.add(std::make_unique<
                      compiler::midend::pass::GuardedInitializationSinkingPass>());
    }
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
