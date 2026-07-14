# 2026 编译系统实现赛 SysY 编译器

本项目实现一个面向 2026 全国大学生计算机系统能力大赛编译系统实现赛 RISC-V 赛道的 SysY 编译器。`wz` 分支持续加入经过正确性、性能和比赛合规性审查的通用优化。

编译链路：

```text
SysY2022/SysY2026 源文件
  -> Lexer / Parser / AST
  -> SemanticAnalyzer / TypeChecker
  -> LLVM IR 风格中间表示
  -> -O1 中端优化
  -> RISC-V 64 指令选择、寄存器分配、栈帧生成
  -> GNU 汇编
```

## 比赛环境

- 编译器实现语言：C++17
- 构建系统：CMake 3.20+、Ninja
- 官方构建环境：Ubuntu 24.04、linux/amd64、Clang/Clang++
- 目标平台：64 位 RISC-V
- 汇编要求：兼容 GCC `-mcmodel=medany`
- 本地运行器：`qemu-riscv64`

Docker 镜像包含 Clang、CMake、Ninja、RISC-V/ARM 交叉工具链、QEMU、Java 和 Rust，可用于复现比赛构建环境。

## 当前实现状态

- 前端：词法、语法、AST、语义分析、类型检查已打通。
- IR：支持控制流、数组、函数调用、整型/浮点运算、类型转换和全局对象。
- `-O1`：运行 Mem2Reg、通用小函数内联、直接尾递归消除、全局标量局部化、过程间副作用分析、公共子表达式消除、循环不变量外提、二次幂余数比较简化、冗余 load/store 消除、死代码删除和空基本块清理。
- 后端：支持 RISC-V 64 指令选择、Phi 消除、栈对象提升、嵌套循环全局地址物化提升、图着色寄存器分配、机器级死代码消除、栈帧与汇编输出。
- 计时函数：`starttime()` / `stoptime()` 会被降级为带源码行号的 `_sysy_starttime(int)` / `_sysy_stoptime(int)` 调用。

## 编译优化合规声明

- 编译器不读取或匹配输入文件名、测试用例名、源代码特定字符串、特定函数/变量名、计时调用行号、公开输入数据或已知输出。
- 已删除以公开用例的整函数形状、固定参数个数、固定全局对象、固定 CFG 标签或运行时输入范围激活的专项 pass；也已删除仅覆盖 `1000`、`65535`、`998244353` 的后端常数特判。
- 保留的优化只依据通用 IR/Machine IR 语义、数据流、控制流、别名/副作用信息以及 RISC-V ISA/BOOM 的通用硬件特性，并要求在所有满足 SysY 语义的输入上保持正确。
- 初赛 RISC-V 目标不生成 SIMD/向量指令；只使用技术方案允许的 RV64GC 能力。

## 人工智能辅助与技术参考披露

- 开发过程中使用 OpenAI Codex 辅助进行源码审查、优化方案分析、C++ 实现与重构、测试脚本和项目文档编写。参赛队员负责确定需求、审查和修改生成代码、处理集成冲突、运行正确性/性能测试，并决定最终保留或撤销的实现。
- 优化思路曾参考 2020 年 USTC [FlammingMyCompiler](https://github.com/mlzeng/CSC2020-USTC-FlammingMyCompiler) 与 2021 年 [MahoShojo](https://github.com/wildoranges/MahoShojo) 等公开获奖项目，以及公开的经典编译原理和 RISC-V ABI/ISA 资料；本仓库未直接复制 GCC、LLVM 或上述项目源码。借鉴的具体技术包括数据流优化、循环优化、指令选择和图着色寄存器分配，其实现均结合本项目 IR 与后端独立完成。

## 验证记录

最新的有效、退化、未命中、不完整和违规实验分类见 [优化实验与消融记录](docs/optimization-experiment-log.md)。已删除的专项方案只保留在该文档的 E 类历史中，不属于当前编译器能力。

- Ubuntu 24.04/amd64 比赛容器内 Release 构建成功。
- `correctness` 通道最近一次完整回归为 140/140 AC。
- 当前 `zct` 的 `performance` 通道为 60/60 AC，2026-07-14 同容器逐用例内部计时总和从 `7.357633s` 降至 `7.129345s`，约降低 `3.10%`。本地 QEMU 计时只用于版本间回归，不代表官方 BOOM 评测机时间。
- 当前性能分析入口可记录计时区间动态指令、最热基本块及 RISC-V `generic-ooo` 近似调度结果；这些分析运行不参与计时。

## 项目结构

```text
compiler/
├── benchmarks/nscscc-implementation/2026/
│   ├── functional/       # 100 个公开功能用例
│   ├── h_functional/     # 40 个进阶功能用例
│   └── performance/      # 60 个性能用例
├── docker/impl2026/Dockerfile
├── include/compiler/
├── scripts/
│   ├── build_impl2026.sh
│   ├── dev_impl2026.sh
│   └── eval.py           # 唯一的本地评测入口
├── src/
└── tests/
```

## 初始化开发环境

构建比赛环境镜像：

```bash
docker build \
  --platform linux/amd64 \
  -t compiler-impl2026-env \
  -f docker/impl2026/Dockerfile .
```

创建常驻开发容器：

```bash
docker run -dit \
  --platform linux/amd64 \
  --name compiler-impl2026-dev \
  -v "$(pwd):/workspace" \
  -w /workspace \
  compiler-impl2026-env:latest \
  sleep infinity
```

后续使用：

```bash
docker start compiler-impl2026-dev
docker exec -it compiler-impl2026-dev bash
```

## 构建编译器

在开发容器内执行：

```bash
cmake -S /workspace -B /workspace/.docker-build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++

cmake --build /workspace/.docker-build
```

也可以从宿主机直接执行：

```bash
docker exec compiler-impl2026-dev bash -lc '
  cmake -S /workspace -B /workspace/.docker-build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ &&
  cmake --build /workspace/.docker-build
'
```

## 编译器用法

官方功能测试调用形式：

```bash
/workspace/.docker-build/compiler -S -o testcase.s testcase.sy
```

官方性能测试调用形式：

```bash
/workspace/.docker-build/compiler -S -o testcase.s testcase.sy -O1
```

其他常用命令：

```bash
# 输出 RISC-V 汇编
/workspace/.docker-build/compiler input.sy -o output.s

# 查看 Token、AST 或 IR
/workspace/.docker-build/compiler input.sy --dump-tokens
/workspace/.docker-build/compiler input.sy --dump-ast
/workspace/.docker-build/compiler input.sy --dump-ir
```

检查汇编能否被 RISC-V 工具链接受：

```bash
riscv64-linux-gnu-gcc \
  -march=rv64gc \
  -mcmodel=medany \
  -c output.s \
  -o /tmp/output.o
```

## 图着色寄存器分配

后端在 Phi 消除和栈对象提升之后执行全局图着色寄存器分配，整数寄存器类和浮点寄存器类独立着色。主要流程如下：

1. 基于 CFG、虚拟寄存器编号位向量和反向工作队列求解基本块 `live-in` / `live-out`，只在后继活跃集合变化时重新处理前驱，再反向扫描每条 Machine IR 指令得到精确活跃集合。
2. 为同时活跃的虚拟寄存器建立冲突边。普通和稠密函数使用连续压缩位矩阵；预计矩阵超过内存预算时自动切换到稀疏邻接，并在建图后压缩为有序邻接表。
3. 精确识别跨函数调用存活的值，优先为它们分配 callee-saved 寄存器；寄存器压力过高时允许 caller-saved 兜底，并依据每条调用指令的精确活跃集合只保存和恢复真正跨越该调用点的值。调用参数和返回值不再被错误地视为跨调用值。
4. 对不跨调用的整数入口参数使用 ABI 参数寄存器预着色；调用实参使用带循环破除的并行搬运，并在覆盖参数寄存器前先写出栈参数。
5. Copy 指令建立带自然循环权重的颜色亲和关系，调用实参、调用结果和函数返回值同时建立 ABI 寄存器偏好；着色时选择动态搬运收益最大的可用颜色。汇编输出会彻底删除同色 Copy，并对不同颜色直接生成单条寄存器搬运。
6. 使用“简化、潜在溢出、选择着色”的乐观图着色流程。颜色约束、邻居禁色和 Copy 偏好全部使用 64 位寄存器掩码，简化阶段增量维护节点度数，避免逐节点字符串集合和寄存器排序。
7. 溢出代价综合使用/定义次数、自然循环深度和节点度数，优先保留热循环中的值；自然循环通过连续支配位矩阵和无哈希工作栈识别。
8. 无法着色的值复用统一 spill slot 机制，在使用点插入 reload，在定义点插入 store。
9. 汇编输出直接使用图着色得到的物理寄存器完成算术、比较、转换、load/store、分支和 GEP；仅立即数或栈值使用保留 scratch，并使用 `addw/subw/mulw/divw/remw/negw` 合并 32 位运算后的符号扩展。
10. 所有地址不逃逸的整数标量局部量都参与栈对象提升，不再要求必须定义在入口块，因此循环体内的 `j/k/sum/val` 等热变量可以直接着色到寄存器；提升后释放对应帧空间。无调用、无有效栈对象、无栈参数且不使用 callee-saved 的叶函数完全省略栈帧及 `ra/s0` 保存恢复。

循环深度的支配关系分析同样使用位向量，着色简化阶段增量维护节点度数，因此大型函数不会退化为反复扫描整张图。需要查看单个函数各阶段开销时，可以执行：

```bash
COMPILER_REGALLOC_STATS=1 /workspace/.docker-build/compiler input.sy -O1 -o output.s
```

## 统一的 2026 本地评测

项目只保留一个评测脚本：[scripts/eval.py](scripts/eval.py)。脚本应在 `compiler-impl2026-dev` 容器内运行。

未传 `--compiler` 时，脚本会在 `.docker-build/compiler`、`build/compiler` 和 `.local-build/compiler` 中选择修改时间最新的可执行文件，避免误用旧构建；需要完全固定评测对象时可显式传入 `--compiler build/compiler`。

它使用比赛页面和官方仓库公开的 2026 RISC-V 用例：

- `correctness` 通道：100 个 `functional` + 40 个 `h_functional`。
- `performance` 通道：60 个 `performance`。
- `all`：先运行正确性通道，再运行性能通道。

脚本对每个用例执行：

1. 按官方命令生成 RISC-V 汇编；性能通道启用 `-O1`。
2. 使用 `riscv64-linux-gnu-gcc -static -march=rv64gc -mcmodel=medany` 链接公开 SysY 运行时。
3. 使用 `qemu-riscv64` 执行。
4. 将标准输出和程序返回值与 `.out` 对拍。
5. 性能通道从标准错误解析 `TOTAL: xH-xM-xS-xus`。
6. 记录编译、链接、运行墙钟时间和程序内部计时。
7. 解析生成汇编，记录指令、load/store、栈访存、分支、调用、搬运、乘除和向量指令数量，并用汇编回边估算热循环加权指令、访存与分支规模。
8. 可选地另起一次插件化 QEMU 运行，记录动态指令总数以及最热基本块的地址、指令数、执行次数和动态指令占比。该次运行不参与性能计时。
9. 可选地反汇编最热基本块，并用 LLVM 21 的 RISC-V `generic-ooo` 模型记录 IPC、稳态吞吐和近似调度周期。

### 正确性入口

```bash
python3 scripts/eval.py correctness
```

宿主机直接调用：

```bash
docker exec compiler-impl2026-dev bash -lc \
  'cd /workspace && python3 scripts/eval.py correctness'
```

### 性能入口

```bash
python3 scripts/eval.py performance
```

### 全部入口

```bash
python3 scripts/eval.py all
```

### 小规模或单用例验证

```bash
python3 scripts/eval.py correctness --limit 5
python3 scripts/eval.py performance --sample optimization_scheduling2.sy
python3 scripts/eval.py performance --sample performance/matmul2.sy
```

### 与上一版汇编逐用例对比

```bash
python3 scripts/eval.py performance \
  --baseline-results .eval-2026/上一版/results.tsv \
  --output-dir .eval-2026/当前方案
```

评测会额外生成 `comparison.tsv`，同时给出内部计时、全部 `asm_*` 指标，以及已启用的动态指令、热点基本块和 MCA 指标的前后值与百分比变化。由于 QEMU 单次时间存在噪声，这些指标用于确认提速是否来自真实的指令、访存、分支或调度开销减少。

### 动态指令与乱序调度分析

保留的 `compiler-impl2026-dev` 容器已经安装：

- 插件版 QEMU 8.2.2：`/opt/qemu-8.2.2-plugin/bin/qemu-riscv64`
- 计时区间动态分析插件：`/opt/qemu-8.2.2-plugin/lib/qemu/plugins/libtimedprofile.so`
- LLVM MCA 21：`llvm-mca-21`，包含 RISC-V `generic-ooo` 模型

只记录动态指令和热点基本块：

```bash
python3 scripts/eval.py performance \
  --sample many_mat_cal-1 \
  --analysis dynamic
```

同时分析最热基本块的近似乱序调度：

```bash
python3 scripts/eval.py performance \
  --sample many_mat_cal-1 \
  --analysis full
```

`--analysis` 不改变正常评测口径：脚本总是先用普通 QEMU 完成计时和对拍，再单独运行插件化 QEMU。动态插件从 `_sysy_starttime` 返回后的第一条指令开始计数，在进入 `_sysy_stoptime` 时停止，只记录与程序内部计时一致的区间，避免输入输出和 C 运行时热点污染结果。插件会显著扰动运行时间，因此 `dynamic_analysis_wall_sec` 仅表示分析成本，不能作为性能成绩。

当前版本对 `many_mat_cal-1` 的计时区间动态指令数从 `1,075,393,213` 降至 `1,005,454,155`，减少 `69,939,058`；最热基本块的 `generic-ooo` 结果从 `3.12` 降至 `2.87 cycles/次`。该收益来自把深层自然循环中反复物化的全局地址提升到外层预头，触发条件不包含符号名称、数组大小或用例常数。该模型不包含官方 BOOM 的真实缓存、TLB、分支预测器和内存延迟，只用于同一用例不同编译器版本之间的相对比较。

### 超时和产物选项

```bash
python3 scripts/eval.py performance \
  --compile-timeout 120 \
  --link-timeout 120 \
  --run-timeout 600

python3 scripts/eval.py correctness --keep-artifacts
```

默认输出目录：

```text
/workspace/.eval-2026/<timestamp>/
```

主要结果文件：

- `results.tsv`：逐用例状态、计时、全部 `asm_*` 指标，以及可选的 `dynamic_*`、`hot_block_*` 和 `mca_*` 指标。
- `comparison.tsv`：指定 `--baseline-results` 时生成的逐用例计时、汇编、动态指令和调度差异。
- `summary.txt`：适合终端查看的汇总。
- `summary.json`：机器可读汇总。
- `analysis/`：动态插件输出、热点反汇编、热点汇编和 LLVM MCA 完整报告。
- `failures/`：失败用例的源码、stdout、stderr、期望输出和实际输出。
- `runtime/sylib.c`：本次评测使用的公开 SysY 运行时副本。

注意：QEMU 时间只适合本地回归和不同提交之间的粗略比较，不能替代官方 FPGA BOOM CPU 开发板成绩。平台上一次 AC 记录中的 `1582.7551s` 是 60 个 2026 性能用例在官方开发板上的计时总和。

官方 BOOM FPGA 的开发板申请、macOS 端口转发、容器内裸机 ELF 构建、GDB 下载运行、周期读取和每轮重置步骤见 [docs/fpga-performance.md](docs/fpga-performance.md)。可复用的裸机运行时与命令入口位于 `scripts/fpga/`。

2026-07-14 的 60 个性能测试点、榜上最佳时间和差距快照见 [docs/performance-leaderboard-snapshot.md](docs/performance-leaderboard-snapshot.md)。该表只用于选择通用编译器热点，所有优化仍须遵守 `AGENTS.md` 的比赛合规门禁。

## 当前限制

- 仅支持单输入文件。
- 类型检查和语义分析仍有部分语言边界未完整覆盖。
- IR 尚未形成完整 use-def 链，优化框架仍在演进。
- 当前只维护完整的 `riscv64` 后端链路。
- 当前比赛链接口径为 `rv64gc`，不包含 RISC-V Vector Extension，因此不能直接生成 RVV 指令；可继续采用标量循环展开、软件流水、访存合并和指令调度。
- 本地性能评测依赖 QEMU，数值与官方硬件不可直接比较；`generic-ooo` 同样只是乱序流水线近似模型，不模拟官方 BOOM 的缓存和内存系统。
