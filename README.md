# SysY 编译器骨架项目

## 项目简介

本项目是一个面向编译系统实现赛的 **SysY 编译器骨架**。当前版本已经完成并打通前端主干与一条可运行的 `riscv64` 后端链路：

`源文件 -> 词法分析 -> 语法分析 -> 结构化 AST -> 语义分析 -> 类型检查 -> AST 驱动 pseudo-IR Lowering -> IR Verifier -> 指令选择 -> 栈式寄存器分配 -> 栈帧生成 -> RISC-V 汇编输出`

当前阶段的定位是：

1. 以前端为核心的可扩展开发基线
2. 词法分析与语法分析已具备独立调试和回归能力
3. 中端优化仍待继续实现；`riscv64` 后端已能消费当前 IR 子集并输出可汇编的目标代码

## 默认环境

当前仓库默认依托你提供的 **2026 编译系统实现赛官方环境** 开发和构建。

对应的默认口径是：

1. 编译器使用 `C / C++ / Java / Rust` 之一开发
2. 编译器源码在 `x86 / AMD64` 评测服务器上编译
3. 评测服务器操作系统：`Ubuntu 24.04 (64位)`
4. ARM 目标平台：输出 `ARMv8-A` 64 位汇编，并在 `XCZU15EG ARM Cortex-A53 + Ubuntu 22.04` 上运行
5. RISC-V 目标平台：输出 `64位 RISC-V` 汇编，并满足 `-mcmodel=medany` 约定后在 `FPGA BOOM CPU` 软核上运行

## 技术栈

1. 语言：C++17
2. 构建工具：CMake 3.20+
3. 默认构建环境：`linux/amd64 + Ubuntu 24.04 + clang/clang++ + Ninja`
4. 依赖：标准 C++ 库，无第三方运行时依赖
5. 当前目标输出：RISC-V / AArch64 汇编文本

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
| 语义分析 | 已实现全局/局部符号表、函数声明收集、重复定义检查、未定义标识符检查、内建函数注册与调用参数个数检查、函数声明签名一致性检查、数组形参数组层级检查、`break/continue` 上下文检查、`switch-case-default` 合法性检查、`goto/label` 解析、`void` 返回值误用检查、`return` 语句合法性检查、整数常量表达式求值、数组维度常量性/正数性检查、`case` 标签常量性检查、全局初始化常量表达式检查、递归初始化列表常量性检查、常量零除检查 | `src/frontend/sema/SemanticAnalyzer.cpp`、`SemanticAnalyzer` | Partial |
| 类型系统 | 已定义 `void / int / float / bool / long / unknown` 类型枚举与字符串化接口，并在类型检查阶段跟踪数组层级与已知维度信息 | `include/compiler/frontend/types/Type.h`、`src/frontend/types/TypeChecker.cpp` | Partial |
| 类型检查 | 已实现表达式类型推导、数组/标量区分、函数调用参数类型检查、赋值兼容性检查、返回值类型检查、条件表达式类型检查、下标表达式检查、算术/逻辑/比较/位运算约束检查、`const` 赋值检查、数值类型在初始化/赋值/调用/返回中的隐式转换、标量花括号初始化、数组初始化列表的按层对齐/补零规则、数组初始化列表元素类型检查、数组整体容量与子数组边界越界检查、数组实参与形参数组尾部维度匹配检查，并将归一化后的线性初始化槽位结果回写到 AST 变量声明缓存 | `src/frontend/types/TypeChecker.cpp`、`TypeChecker` | Partial |
| IR 核心结构 | 已定义 `Module / GlobalVariable / Constant / Function / BasicBlock / Instruction / Value` 等基础 IR 数据结构；全局变量为结构化 typed global object，函数带返回类型与参数签名，指令结果值不再内嵌在 opcode 字符串中 | `include/compiler/ir/core/*.h` | Done |
| IR Builder | 已提供基础 block 创建与指令发射接口 | `include/compiler/ir/builder/IRBuilder.h` | Done |
| IR Lowering | 已实现 AST 到 textual pseudo-IR 的系统化 lowering，覆盖结构化全局变量初始化、局部声明与归一化数组初始化、标量/数组表达式、隐式数值转换、赋值、函数调用、`if / while / do-while / for / break / continue / switch / case / default / goto / label / return`、短路逻辑与基本块控制流生成 | `src/ir/builder/AstToIR.cpp`、`AstToIR` | Done |
| IR 打印 | 支持模块、函数、基本块、IR 指令输出 | `src/ir/printer/IRPrinter.cpp`、`IRPrinter` | Done |
| IR 验证 | 已实现模块级 IR verifier，检查全局/函数/基本块命名唯一性、空块、终结指令位置、`jmp/br/ret` 形状与跳转目标合法性、返回值与函数签名一致性 | `src/ir/verify/IRVerifier.cpp`、`IRVerifier` | Done |
| 中端 Pass 管理 | 支持注册与执行 `ModulePass`；`-O1` 当前仅注入占位 pass | `src/midend/pass/PassManager.cpp`、`PassManager` | Partial |
| 目标机描述 | 支持 `riscv64` 与 `aarch64` 目标信息 | `src/backend/target/TargetInfo.cpp`、`TargetInfo` | Done |
| 指令选择 | 已实现 IR 到机器层 lowering，覆盖 `alloca / load / store / jmp / br / ret / call / gep / gep.linear`、整型/浮点算术、整型/浮点比较、`cast / zext / sitofp / uitofp / fptosi`，并保留全局对象/参数/返回类型信息 | `src/backend/isel/InstructionSelector.cpp`、`InstructionSelector` | Done |
| 栈帧生成 | 已实现栈对象布局、对齐、frame size 计算、outgoing argument area 计算，并为序言/尾声生成偏移信息 | `src/backend/frame/FrameLowering.cpp`、`FrameLowering` | Done |
| 寄存器分配 | 已实现可运行的栈式寄存器分配器，将虚拟值和形参 home slot 分配到栈槽 | `src/backend/regalloc/RegisterAllocator.cpp`、`RegisterAllocator` | Done |
| 汇编输出 | 已实现 `riscv64` 汇编输出，覆盖全局区、函数序言/尾声、整数/浮点运算、比较、跳转、调用、数组寻址与返回；`aarch64` 仍未补齐 | `src/backend/asm/AsmPrinter.cpp`、`AsmPrinter` | Partial |
| 前端样例回归 | 已纳入基础样例、数组样例、浮点样例、扩展前端覆盖样例、语义分析正反样例与类型检查样例；新增全局数组初始化、数组初始化过量、标量花括号初始化、数组形参数组尾部维度错误样例；已完成本地构建回归 | `tests/e2e/frontend/*.sy`、`tests/e2e/functional/frontend_full.sysy`、`tests/e2e/functional/return_42.sysy`、`tests/e2e/sema/*.sy`、`tests/e2e/types/*.sy` | Done |

## 项目结构说明

```text
compiler/
├── CMakeLists.txt
├── CMakePresets.json
├── docker/
│   └── impl2026/
│       └── Dockerfile
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
├── scripts/
│   ├── build_impl2026.sh
│   └── dev_impl2026.sh
├── src/
├── runtime/
├── tools/
├── benchmarks/
└── tests/
```

## 构建与使用

### 进入默认环境

```bash
bash scripts/dev_impl2026.sh
```

### 在默认环境中构建

```bash
cmake --preset impl2026
cmake --build --preset impl2026
```

### 一步完成容器内构建

```bash
bash scripts/build_impl2026.sh
```

### 查看帮助

```bash
./build/compiler --help
```

### 查看最小样例的 token / AST / IR

```bash
./build/compiler tests/e2e/functional/return_42.sysy --dump-tokens --dump-ast --dump-ir
```

### 生成 RISC-V 汇编

```bash
./build/compiler tests/e2e/functional/return_42.sysy -o build/return_42.s
```

### 检查 RISC-V 输出满足较大地址空间约定

```bash
riscv64-linux-gnu-gcc -march=rv64gc -mcmodel=medany -c build/return_42.s -o /tmp/return_42.o
```

### 生成 AArch64 汇编

```bash
./build/compiler tests/e2e/functional/return_42.sysy --target aarch64 -o build/return_42_arm.s
```

## 当前限制

1. 仅支持单输入文件
2. 类型检查仍未覆盖显式类型转换、指针相关规则、字符串字面量的完整类型约束，以及聚合初始化结果的归一化产出
3. 语义分析仍未覆盖更多声明兼容规则与更完整的常量折叠结果复用
4. IR 仍是 textual pseudo-IR，虽然已具备结构化全局对象与 verifier，但尚未进行 SSA 化，也还没有完整的 use-def / `phi` 建模
5. 中端暂无真实优化 pass
6. 后端当前完整实现聚焦 `riscv64`；`aarch64` 仍未补齐到同等完成度
