# SysY 编译器骨架项目

## 项目简介

本项目是一个面向编译系统实现赛的 **SysY 编译器骨架**。当前版本已经打通了最小可运行闭环：

`源文件 -> 词法分析 -> AST 壳结构 -> 语义/类型检查占位 -> IR Lowering -> 后端指令选择 -> 汇编输出`

当前可验证的目标能力如下：

1. 读取单个输入源文件
2. 输出 token 流、AST 摘要、IR 文本
3. 将最小模式 `return <整数常量>;` 转换为目标汇编
4. 输出 `riscv64` 或 `armv7` 汇编

当前版本的定位是 **后续开发基线**，不是完整编译器实现。

## 技术栈

1. 语言：C++17
2. 构建工具：CMake 3.20+
3. 编译器依赖：标准 C++ 库，无第三方运行时依赖
4. 当前已验证构建环境：AppleClang 17
5. 目标输出：RISC-V / ARMv7 汇编文本

## 当前开发进度

| 模块 | 已实现的核心功能 | 对应文件/类 | 状态 |
| :--- | :--- | :--- | :--- |
| 构建系统 | 定义 `compiler_lib` 静态库与 `compiler` 可执行程序；组织当前所有源码编译 | `CMakeLists.txt` | Done |
| 驱动层 | 支持命令行解析、单文件编译流程调度、诊断输出、`--dump-tokens / --dump-ast / --dump-ir / --target / -O0 / -O1 / -o` | `src/driver/Driver.cpp`、`CompilerInvocation`、`Pipeline`、`Driver` | Done |
| 诊断系统 | 支持 `note / warning / error` 诊断记录与打印 | `src/support/Diagnostic.cpp`、`include/compiler/support/Diagnostic.h` | Done |
| 文件系统 | 支持文本文件读取与输出文件写入 | `src/support/FileSystem.cpp` | Done |
| 词法分析 | 支持关键字 `int / return / if / else / while`、标识符、整数字面量、单字符符号、空白跳过、EOF token、token dump | `src/frontend/lexer/Lexer.cpp`、`Lexer`、`Token` | Done |
| AST 基础结构 | 已定义 `AstNode / TranslationUnit / AstVisitor`，可保存 token 流 | `include/compiler/frontend/ast/AstNode.h` | Partial |
| 语法分析 | 当前仅将 token 流包装为 `TranslationUnit`；未实现函数、语句、表达式级语法分析 | `src/frontend/parser/Parser.cpp`、`Parser` | Partial |
| AST 打印 | 输出 `TranslationUnit` 摘要与 token 数量 | `src/frontend/ast/AstPrinter.cpp`、`AstPrinter` | Done |
| 语义分析 | 仅定义检查入口，当前直接返回成功，未实现符号表、作用域、语义规则 | `src/frontend/sema/SemanticAnalyzer.cpp`、`SemanticAnalyzer` | Partial（仅定义接口） |
| 类型系统 | 已定义 `Type / TypeKind` 及字符串化接口 | `include/compiler/frontend/types/Type.h`、`src/frontend/types/TypeChecker.cpp` | Partial |
| 类型检查 | 仅定义检查入口，当前直接返回成功，未实现类型推导与约束检查 | `src/frontend/types/TypeChecker.cpp`、`TypeChecker` | Partial（仅定义接口） |
| IR 核心结构 | 已定义 `Module / Function / BasicBlock / Instruction / Value` | `include/compiler/ir/core/*.h` | Done |
| IR Builder | 已提供基础 block 创建与指令发射接口 | `include/compiler/ir/builder/IRBuilder.h` | Done |
| IR Lowering | 当前通过扫描 token 流识别 `return <number>`，生成单函数、单基本块、单 `ret` 指令 IR；未基于真实 AST 降低 | `src/ir/builder/AstToIR.cpp`、`AstToIR` | Partial |
| IR 打印 | 输出模块、函数、基本块、IR 指令与 pass 日志 | `src/ir/printer/IRPrinter.cpp`、`IRPrinter` | Done |
| 中端 Pass 管理 | 支持注册 `ModulePass` 并执行；`-O1` 时仅注入 `pipeline-marker` 占位 pass；无真实优化实现 | `src/midend/pass/PassManager.cpp`、`PassManager` | Partial |
| 目标机描述 | 支持 `riscv64` 与 `armv7` 目标信息构建 | `src/backend/target/TargetInfo.cpp`、`TargetInfo` | Done |
| 指令选择 | 当前仅支持将 IR 中的 `ret <imm>` 映射到目标相关机器指令序列 | `src/backend/isel/InstructionSelector.cpp`、`InstructionSelector` | Partial |
| 栈帧生成 | 已保留后端阶段入口，当前无实际逻辑 | `src/backend/frame/FrameLowering.cpp`、`FrameLowering` | Partial（仅定义接口） |
| 寄存器分配 | 已保留后端阶段入口，当前无实际逻辑 | `src/backend/regalloc/RegisterAllocator.cpp`、`RegisterAllocator` | Partial（仅定义接口） |
| 汇编输出 | 支持输出最小 RISC-V/ARMv7 汇编；RISC-V 使用 `li a0, imm` + `ret`，ARMv7 使用 `mov r0, #imm` + `bx lr` | `src/backend/asm/AsmPrinter.cpp`、`AsmPrinter` | Done |
| 端到端样例 | 已提供最小功能样例 `return 42`，并完成 RISC-V / ARMv7 输出验证 | `tests/e2e/functional/return_42.sysy` | Done |

## 项目结构说明

```text
compiler/
├── CMakeLists.txt           # 构建入口
├── include/compiler/        # 头文件，按模块组织公共接口
├── src/                     # 实现代码
│   ├── main/                # 程序入口
│   ├── driver/              # 命令行、编译流程调度
│   ├── support/             # 诊断、文件读写等基础设施
│   ├── frontend/            # 词法、语法、AST、语义、类型
│   ├── ir/                  # IR 核心结构、Lowering、IR 打印
│   ├── midend/              # Pass 管理层；当前仅保留最小接口
│   └── backend/             # 目标机、指令选择、栈帧、寄存器分配、汇编输出
├── tests/                   # 测试目录；当前已有最小 e2e 样例
├── docs/                    # 设计文档目录
├── runtime/                 # 运行时相关目录，当前未接入实际实现
├── scripts/                 # 构建/测试脚本目录，当前未添加脚本
├── tools/                   # 开发辅助工具目录，当前未添加工具
├── benchmarks/              # 性能测试目录，当前未添加内容
├── 调研结果.md              # 比赛与赛道调研文档
└── 编译器架构与项目规划.md  # 项目结构规划文档
```

### 当前最关键入口

1. `src/driver/Driver.cpp`：主编译流水线
2. `src/frontend/lexer/Lexer.cpp`：当前最完整的前端实现
3. `src/ir/builder/AstToIR.cpp`：当前 IR 生成逻辑
4. `src/backend/isel/InstructionSelector.cpp`：当前后端模式选择逻辑
5. `src/backend/asm/AsmPrinter.cpp`：当前目标汇编输出逻辑

## 队友接手指南（TODO List）

以下任务按优先级排序，建议按顺序推进。

- [ ] 实现真正的语法分析和 AST 节点体系  
  切入点：从 `src/frontend/parser/Parser.cpp:32` 开始，当前 `Parser::parse()` 仅返回 `TranslationUnit`。  
  具体建议：先扩展 `include/compiler/frontend/ast/AstNode.h`，增加 `FunctionDecl`、`BlockStmt`、`ReturnStmt`、`IntegerLiteral` 等节点；再在 `Parser::parse()` 中拆出 `parseFunction()`、`parseBlock()`、`parseStmt()`、`parseExpr()`。

- [ ] 用真实 AST 驱动 IR Lowering，替换当前 token 扫描逻辑  
  切入点：从 `src/ir/builder/AstToIR.cpp:12` 的 `inferReturnValue()` 和 `src/ir/builder/AstToIR.cpp:25` 的 `AstToIR::lower()` 开始。  
  具体建议：删除对 token 序列的直接扫描，改为遍历 AST 节点生成 IR；第一阶段先支持 `int main() { return <expr>; }`，之后逐步扩展变量、二元表达式和控制流。

- [ ] 扩展后端为真实函数级代码生成  
  切入点：从 `src/backend/isel/InstructionSelector.cpp:18` 开始，当前仅处理 `ret <imm>`；随后补全 `src/backend/frame/FrameLowering.cpp:5` 和 `src/backend/regalloc/RegisterAllocator.cpp:5`。  
  具体建议：先支持表达式求值结果落到返回寄存器，再补函数序言/尾声、栈槽分配和最小寄存器分配策略，确保后续支持局部变量与临时值。

## 编译与测试

### 构建

```bash
cmake -S . -B build
cmake --build build -j4
```

### 查看帮助

```bash
./build/compiler --help
```

### 运行最小样例并输出调试信息

```bash
./build/compiler tests/e2e/functional/return_42.sysy --dump-tokens --dump-ast --dump-ir -O1 -o build/return_42.s
```

### 生成 RISC-V 汇编

```bash
./build/compiler tests/e2e/functional/return_42.sysy -o build/return_42.s
```

### 生成 ARMv7 汇编

```bash
./build/compiler tests/e2e/functional/return_42.sysy --target armv7 -o build/return_42_arm.s
```

### 当前已验证的样例输出

输入文件：

```c
int main() {
  return 42;
}
```

RISC-V 输出：

```asm
.text
.globl main
main:
  li a0, 42
  ret
```

ARMv7 输出：

```asm
.text
.globl main
main:
  mov r0, #42
  bx lr
```

## 当前限制

1. 仅支持单输入文件
2. 词法分析未处理注释、字符串、浮点数、多字符运算符
3. 语法分析未实现真实文法
4. 语义分析与类型检查当前为占位实现
5. IR Lowering 当前仅支持从 token 中提取 `return <整数常量>`
6. 中端无实际优化逻辑
7. 后端仅支持最小返回路径，不支持局部变量、表达式求值、分支、调用约定和真实寄存器分配

## 开发原则

1. 新增功能时，优先保证 `tests/e2e/functional/return_42.sysy` 继续可编译
2. 每新增一种 AST 节点，同时补对应的 IR Lowering 逻辑
3. 每新增一种 IR 指令，同时补齐指令选择与汇编打印路径
4. 中端优化在前端 AST 与后端最小功能稳定后再逐步加入
