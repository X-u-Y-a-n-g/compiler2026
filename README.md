# SysY 编译器骨架项目

## 项目简介

本项目是一个面向编译系统实现赛的 **SysY 编译器骨架**。当前版本已经完成并打通前端主干：

`源文件 -> 词法分析 -> 语法分析 -> 结构化 AST -> 语义/类型检查占位 -> AST 驱动 IR Lowering -> 最小后端汇编输出`

当前阶段的定位是：

1. 以前端为核心的可扩展开发基线
2. 词法分析与语法分析已具备独立调试和回归能力
3. 中端优化、语义规则、完整后端仍待继续实现

## 技术栈

1. 语言：C++17
2. 构建工具：CMake 3.20+
3. 依赖：标准 C++ 库，无第三方运行时依赖
4. 已验证构建环境：AppleClang 17
5. 当前目标输出：RISC-V / ARMv7 汇编文本

## 当前开发进度

| 模块 | 已实现的核心功能 | 对应文件/类 | 状态 |
| :--- | :--- | :--- | :--- |
| 构建系统 | 定义 `compiler_lib` 静态库与 `compiler` 可执行程序，组织全部源码编译 | `CMakeLists.txt` | Done |
| 驱动层 | 支持单文件编译流程、诊断输出、`--dump-tokens / --dump-ast / --dump-ir / --target / -O0 / -O1 / -o` | `src/driver/Driver.cpp`、`CompilerInvocation`、`Pipeline` | Done |
| 诊断系统 | 支持 `note / warning / error` 诊断记录与打印 | `src/support/Diagnostic.cpp`、`include/compiler/support/Diagnostic.h` | Done |
| 文件系统 | 支持文本文件读取与输出文件写入 | `src/support/FileSystem.cpp` | Done |
| 词法分析 | 支持标识符、整型/长整型判定、十进制/十六进制浮点、字符串、注释、关键字 `int / float / void / bool / long / const / return / if / else / for / while / continue / break / switch / case / default / goto / do`、数组/块/调用相关分隔符、比较/逻辑/位运算/赋值运算符、EOF token 输出 | `src/frontend/lexer/Lexer.cpp`、`include/compiler/frontend/lexer/Token.h`、`Lexer` | Done |
| AST 结构 | 已实现真实语法树节点体系，覆盖翻译单元、函数/变量声明、块、返回、条件、循环、`switch-case-default`、`goto/label`、调用、下标、一元/二元表达式、整数字面量、浮点字面量、字符串字面量 | `include/compiler/frontend/ast/AstNode.h` | Done |
| 语法分析 | 已实现递归下降语法分析；支持顶层变量声明、函数声明/定义、数组声明、参数声明、表达式优先级、逗号表达式、控制流语句、`switch-case-default`、`goto`/标签、调用与下标访问 | `src/frontend/parser/Parser.cpp`、`include/compiler/frontend/parser/Parser.h`、`Parser` | Done |
| AST 打印 | 输出结构化 AST，便于前端调试与回归 | `src/frontend/ast/AstPrinter.cpp`、`AstPrinter` | Done |
| 语义分析 | 保留分析入口，当前未实现符号表、作用域检查、控制流合法性检查 | `src/frontend/sema/SemanticAnalyzer.cpp`、`SemanticAnalyzer` | Partial（仅定义接口） |
| 类型系统 | 已定义 `void / int / float / bool / long / unknown` 类型枚举与字符串化接口 | `include/compiler/frontend/types/Type.h`、`src/frontend/types/TypeChecker.cpp` | Partial |
| 类型检查 | 保留检查入口，当前未实现表达式类型推导、赋值约束、函数签名检查 | `src/frontend/types/TypeChecker.cpp`、`TypeChecker` | Partial（仅定义接口） |
| IR 核心结构 | 已定义 `Module / Function / BasicBlock / Instruction / Value` 等基础 IR 数据结构 | `include/compiler/ir/core/*.h` | Done |
| IR Builder | 已提供基础 block 创建与指令发射接口 | `include/compiler/ir/builder/IRBuilder.h` | Done |
| IR Lowering | 当前基于 AST 识别 `main` 中可静态提取的整数返回路径，生成最小 `ret` IR；复杂表达式和一般语句尚未降低 | `src/ir/builder/AstToIR.cpp`、`AstToIR` | Partial |
| IR 打印 | 支持模块、函数、基本块、IR 指令输出 | `src/ir/printer/IRPrinter.cpp`、`IRPrinter` | Done |
| 中端 Pass 管理 | 支持注册与执行 `ModulePass`；`-O1` 当前仅注入占位 pass | `src/midend/pass/PassManager.cpp`、`PassManager` | Partial |
| 目标机描述 | 支持 `riscv64` 与 `armv7` 目标信息 | `src/backend/target/TargetInfo.cpp`、`TargetInfo` | Done |
| 指令选择 | 当前仅支持将 `ret <imm>` 映射为目标相关机器指令 | `src/backend/isel/InstructionSelector.cpp`、`InstructionSelector` | Partial |
| 栈帧生成 | 保留后端阶段入口，暂无实际逻辑 | `src/backend/frame/FrameLowering.cpp`、`FrameLowering` | Partial（仅定义接口） |
| 寄存器分配 | 保留后端阶段入口，暂无实际逻辑 | `src/backend/regalloc/RegisterAllocator.cpp`、`RegisterAllocator` | Partial（仅定义接口） |
| 汇编输出 | 支持最小 RISC-V / ARMv7 汇编输出 | `src/backend/asm/AsmPrinter.cpp`、`AsmPrinter` | Done |
| 前端样例回归 | 已纳入基础样例、数组样例、浮点样例，以及扩展前端覆盖样例；已完成本地构建回归 | `tests/e2e/frontend/*.sy`、`tests/e2e/functional/frontend_full.sysy`、`tests/e2e/functional/return_42.sysy` | Done |

## 项目结构说明

```text
compiler/
├── CMakeLists.txt
├── include/compiler/
│   ├── backend/
│   ├── driver/
│   ├── frontend/
│   │   ├── ast/
│   │   ├── lexer/
│   │   ├── parser/
│   │   ├── sema/
│   │   └── types/
│   ├── ir/
│   ├── midend/
│   └── support/
├── src/
│   ├── backend/
│   ├── driver/
│   ├── frontend/
│   ├── ir/
│   ├── main/
│   ├── midend/
│   └── support/
├── tests/
│   └── e2e/
│       ├── frontend/              # 参考仓库导入的前端回归样例
│       └── functional/            # 当前项目功能样例
├── docs/
├── runtime/
├── scripts/
├── tools/
├── benchmarks/
├── 调研结果.md
└── 编译器架构与项目规划.md
```

### 关键入口

1. `src/driver/Driver.cpp`：编译流水线入口
2. `src/frontend/lexer/Lexer.cpp`：词法分析实现
3. `src/frontend/parser/Parser.cpp`：递归下降语法分析实现
4. `include/compiler/frontend/ast/AstNode.h`：AST 节点定义
5. `src/ir/builder/AstToIR.cpp`：当前 AST 到 IR 的最小 lowering

## 当前已验证内容

1. `tests/e2e/functional/return_42.sysy` 可输出 AST、IR，并生成可用 RISC-V 汇编
2. `tests/e2e/frontend/simple.sy` 可完成词法分析和语法分析
3. `tests/e2e/frontend/witharray.sy` 可完成数组相关语法分析
4. `tests/e2e/frontend/withfloat.sy` 可完成浮点字面量词法分析和语法分析
5. `tests/e2e/functional/frontend_full.sysy` 可覆盖 `default`、逗号表达式、函数原型、数组下标、调用、标签、`goto`、`for`、`switch`、`do-while`

## 队友接手指南（TODO List）

- [ ] 实现真正的语义分析与符号表  
  切入点：`src/frontend/sema/SemanticAnalyzer.cpp`。  
  优先补齐作用域管理、重复定义检查、`break/continue` 所在上下文检查、`goto/label` 解析关系。

- [ ] 实现真正的类型检查  
  切入点：`src/frontend/types/TypeChecker.cpp`。  
  优先补齐字面量类型、表达式二元运算结果类型、赋值兼容性、函数返回类型检查。

- [ ] 将 AST Lowering 扩展为真实表达式/语句级 IR 生成  
  切入点：`src/ir/builder/AstToIR.cpp`。  
  优先支持局部变量、算术表达式、比较表达式、条件跳转和函数调用，替换当前“仅提取整数返回值”的实现。

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

### 查看最小样例的 token / AST / IR

```bash
./build/compiler tests/e2e/functional/return_42.sysy --dump-tokens --dump-ast --dump-ir -o build/return_42.s
```

### 验证前端基础样例

```bash
./build/compiler tests/e2e/frontend/simple.sy --dump-ast -o /tmp/simple.s
./build/compiler tests/e2e/frontend/witharray.sy --dump-ast -o /tmp/witharray.s
./build/compiler tests/e2e/frontend/withfloat.sy --dump-tokens --dump-ast -o /tmp/withfloat.s
```

### 验证扩展前端样例

```bash
./build/compiler tests/e2e/functional/frontend_full.sysy --dump-tokens --dump-ast -o /tmp/frontend_full.s
```

### 生成 RISC-V 汇编

```bash
./build/compiler tests/e2e/functional/return_42.sysy -o build/return_42.s
```

### 生成 ARMv7 汇编

```bash
./build/compiler tests/e2e/functional/return_42.sysy --target armv7 -o build/return_42_arm.s
```

## 当前限制

1. 仅支持单输入文件
2. 语义分析与类型检查仍为占位实现
3. IR Lowering 仅覆盖最小整数返回路径，尚未支持一般表达式与控制流 lowering
4. 中端暂无真实优化 pass
5. 后端仅支持最小返回路径，不支持局部变量、表达式求值、分支、调用约定和真实寄存器分配
