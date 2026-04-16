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
  src/midend/pass/PassManager.cpp
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

target_include_directories(compiler_lib PUBLIC include)
target_compile_options(compiler_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(compiler src/main/main.cpp)
target_link_libraries(compiler PRIVATE compiler_lib)
