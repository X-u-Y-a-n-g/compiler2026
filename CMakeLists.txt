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
  src/midend/analysis/FunctionEffectAnalysis.cpp
  src/midend/pass/AffineArrayCopyPropagationPass.cpp
  src/midend/pass/AffineModularRecurrencePass.cpp
  src/midend/pass/AArch64IntegerDotReductionPass.cpp
  src/midend/pass/AArch64PureMapRecurrenceVectorizationPass.cpp
  src/midend/pass/AArch64VectorConstantRemainderPass.cpp
  src/midend/pass/ArraySummaryDimensionCollapsePass.cpp
  src/midend/pass/ArrayDimensionLayoutPass.cpp
  src/midend/pass/BitwiseIdiomRecognitionPass.cpp
  src/midend/pass/BoundedModuloReductionPass.cpp
  src/midend/pass/CommonSubexpressionEliminationPass.cpp
  src/midend/pass/ConstantArgumentPropagationPass.cpp
  src/midend/pass/ConstantFoldingPass.cpp
  src/midend/pass/DeadCodeEliminationPass.cpp
  src/midend/pass/DivRemPairStrengthReductionPass.cpp
  src/midend/pass/FunctionInliningPass.cpp
  src/midend/pass/GlobalScalarLocalizationPass.cpp
  src/midend/pass/LocalScalarCleanupPass.cpp
  src/midend/pass/LoopInvariantCodeMotionPass.cpp
  src/midend/pass/LoopInterchangeRowUpdatePass.cpp
  src/midend/pass/LoopScalarPromotionPass.cpp
  src/midend/pass/LoopUnrollAndJamPass.cpp
  src/midend/pass/Mem2RegPass.cpp
  src/midend/pass/MonotonicContinueEliminationPass.cpp
  src/midend/pass/MonotonicLoopBoundPass.cpp
  src/midend/pass/NestedInvariantReductionPass.cpp
  src/midend/pass/PassManager.cpp
  src/midend/pass/PowerOfTwoDispatchPass.cpp
  src/midend/pass/RedundantLoadEliminationPass.cpp
  src/midend/pass/RedundantStoreEliminationPass.cpp
  src/midend/pass/RecursiveInliningPass.cpp
  src/midend/pass/RemainderComparisonPass.cpp
  src/midend/pass/TailRecursionEliminationPass.cpp
  src/midend/pass/TrivialBlockEliminationPass.cpp
  src/midend/pass/WidenedModularMultiplyPass.cpp
  src/backend/common/CFGBuilder.cpp
  src/backend/target/TargetInfo.cpp
  src/backend/target/TargetBackend.cpp
  src/backend/target/TargetLoweringSupport.cpp
  src/backend/target/Riscv64Target.cpp
  src/backend/target/AArch64Target.cpp
  src/backend/isel/InstructionSelector.cpp
  src/backend/opt/AArch64InstructionCombining.cpp
  src/backend/opt/AddressReassociation.cpp
  src/backend/opt/CompareBranchFusion.cpp
  src/backend/opt/MachineDeadCodeElimination.cpp
  src/backend/opt/MachineLoopBlockPlacement.cpp
  src/backend/opt/MachineTraceBlockPlacement.cpp
  src/backend/opt/MachineLoopStrengthReduction.cpp
  src/backend/opt/PhiElimination.cpp
  src/backend/opt/StackObjectPromotion.cpp
  src/backend/regalloc/RegisterAllocator.cpp
  src/backend/frame/FrameLowering.cpp
  src/backend/stack/StackLowering.cpp
  src/backend/asm/AsmPrinter.cpp
  src/backend/asm/AArch64AsmPrinter.cpp
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
