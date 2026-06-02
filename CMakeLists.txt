cmake_minimum_required(VERSION 3.20)
project(compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(compiler_lib
  src/driver/Driver.cpp
  src/support/Diagnostic.cpp
  src/support/FileSystem.cpp
  src/frontend/lexer/Lexer.cpp
  src/frontend/parser/Parser.cpp
  src/frontend/ast/AstPrinter.cpp
  src/frontend/sema/SemanticAnalyzer.cpp
  src/frontend/types/TypeChecker.cpp
  src/ir/builder/AstToIR.cpp
  src/ir/printer/IRPrinter.cpp
  src/ir/verify/IRVerifier.cpp
  src/midend/pass/AggressiveDeadCodeEliminationPass.cpp
  src/midend/pass/BasicAliasAnalysisPass.cpp
  src/midend/pass/ArrayAccessSimplificationPass.cpp
  src/midend/pass/CodeGenPreparePass.cpp
  src/midend/pass/CommonSubexpressionEliminationPass.cpp
  src/midend/pass/CompareSimplificationPass.cpp
  src/midend/pass/ConstantFoldingPass.cpp
  src/midend/pass/DeadCodeEliminationPass.cpp
  src/midend/pass/DeadStoreEliminationPass.cpp
  src/midend/pass/FunctionSpecializationPass.cpp
  src/midend/pass/FunctionInliningPass.cpp
  src/midend/pass/GlobalValueNumberingPass.cpp
  src/midend/pass/GlobalConstPropagationPass.cpp
  src/midend/pass/GlobalDCEPass.cpp
  src/midend/pass/IndVarSimplificationPass.cpp
  src/midend/pass/InternalizePass.cpp
  src/midend/pass/LCSSAPass.cpp
  src/midend/pass/LoadEliminationPass.cpp
  src/midend/pass/LocalScalarCleanupPass.cpp
  src/midend/pass/LowerIntrinsicsPass.cpp
  src/midend/pass/LoopAliasAnalysisPass.cpp
  src/midend/pass/LoopAnalysisPass.cpp
  src/midend/pass/LoopEliminationPass.cpp
  src/midend/pass/LoopInvariantCodeMotionPass.cpp
  src/midend/pass/LoopRotatePass.cpp
  src/midend/pass/LoopSimplificationPass.cpp
  src/midend/pass/LoopStrengthReductionPass.cpp
  src/midend/pass/LoopUnrollPass.cpp
  src/midend/pass/LoopUnswitchPass.cpp
  src/midend/pass/Mem2RegPass.cpp
  src/midend/pass/NameNormalizePass.cpp
  src/midend/pass/PassManager.cpp
  src/midend/pass/PhiSimplificationPass.cpp
  src/midend/pass/PipelineBuilder.cpp
  src/midend/pass/ReassociatePass.cpp
  src/midend/pass/RepeatedAccumulationEliminationPass.cpp
  src/midend/pass/ScalarReplacementPass.cpp
  src/midend/pass/SCEVAnalysisPass.cpp
  src/midend/pass/SelectSimplificationPass.cpp
  src/midend/pass/SparseConditionalConstantPropagationPass.cpp
  src/midend/pass/StoreToLoadForwardingPass.cpp
  src/midend/pass/TreeShakingPass.cpp
  src/midend/pass/TrivialBlockEliminationPass.cpp
  src/backend/common/CFGBuilder.cpp
  src/backend/target/TargetInfo.cpp
  src/backend/target/TargetBackend.cpp
  src/backend/target/TargetLoweringSupport.cpp
  src/backend/target/Riscv64Target.cpp
  src/backend/isel/InstructionSelector.cpp
  src/backend/opt/PhiElimination.cpp
  src/backend/opt/StackObjectPromotion.cpp
  src/backend/regalloc/RegisterAllocator.cpp
  src/backend/frame/FrameLowering.cpp
  src/backend/stack/StackLowering.cpp
  src/backend/asm/AsmPrinter.cpp
)

file(GLOB_RECURSE COMPILER_HEADERS CONFIGURE_DEPENDS include/compiler/*.h include/compiler/*.hpp)
set(COMPILER_INCLUDE_DIRS include)
foreach(header ${COMPILER_HEADERS})
  get_filename_component(header_dir "${header}" DIRECTORY)
  list(APPEND COMPILER_INCLUDE_DIRS "${header_dir}")
endforeach()
list(REMOVE_DUPLICATES COMPILER_INCLUDE_DIRS)

target_include_directories(compiler_lib PUBLIC ${COMPILER_INCLUDE_DIRS})
target_compile_options(compiler_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(compiler src/main/main.cpp)
target_link_libraries(compiler PRIVATE compiler_lib)
