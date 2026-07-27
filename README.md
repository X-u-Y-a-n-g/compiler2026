# 2026 编译系统实现赛 SysY 编译器

本项目实现一个面向 2026 全国大学生计算机系统能力大赛编译系统实现赛的 SysY 编译器。RISC-V 64 是默认比赛目标，同时提供可选择的 AArch64 后端；`wz` 分支持续加入经过正确性、性能和比赛合规性审查的通用优化。

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
- 目标平台：默认 64 位 RISC-V；可选 AArch64（AAPCS64）
- 汇编要求：兼容 GCC `-mcmodel=medany`
- 本地运行器：`qemu-riscv64`

Docker 镜像包含 Clang、CMake、Ninja、RISC-V/ARM 交叉工具链、QEMU、Java 和 Rust，可用于复现比赛构建环境。

## 当前实现状态

- 前端：词法、语法、AST、语义分析、类型检查已打通。
- IR：支持控制流、数组、函数调用、整型/浮点运算、类型转换和全局对象。
- `-O1`：运行 Mem2Reg、通用小函数内联、热调用点的受限完整 CFG 内联、直接尾递归消除、受代码增长预算约束的直接自递归展开、含调用副作用保护的单全局标量循环缓存、过程间副作用分析、计数仿射/模递推闭式化简、公共子表达式消除、循环不变量外提、带依赖证明的标量归约循环展开合并、二次幂余数比较简化、冗余 load/store 消除、死代码删除和空基本块清理。
- 后端：支持并行选择的 RISC-V 64 与 AArch64 目标。两者共享 Phi 消除、栈对象提升、地址重关联、机器级循环地址归纳变量强度削弱、循环布局、图着色寄存器分配和机器级死代码消除；各自拥有独立的寄存器信息、调用约定、栈帧规则与汇编输出。AArch64 后端遵循 AAPCS64，支持整数/浮点传参与返回、栈上传参、callee-saved 保存恢复、全局对象和完整标量指令降级。
- 计时函数：`starttime()` / `stoptime()` 会被降级为带源码行号的 `_sysy_starttime(int)` / `_sysy_stoptime(int)` 调用。

## 编译优化合规声明

- 编译器不读取或匹配输入文件名、测试用例名、源代码特定字符串、特定函数/变量名、计时调用行号、公开输入数据或已知输出。
- 已删除以公开用例的整函数形状、固定参数个数、固定全局对象、固定 CFG 标签或运行时输入范围激活的专项 pass；也已删除仅覆盖 `1000`、`65535`、`998244353` 的后端常数特判。
- 保留的优化只依据通用 IR/Machine IR 语义、数据流、控制流、别名/副作用信息以及 RISC-V ISA/BOOM 的通用硬件特性，并要求在所有满足 SysY 语义的输入上保持正确。
- 初赛 RISC-V 目标不生成 SIMD/向量指令；只使用技术方案允许的 RV64GC 能力。

## 人工智能辅助与技术参考披露

- 开发过程中使用 OpenAI Codex 辅助进行源码审查、优化方案分析、C++ 实现与重构、测试脚本和项目文档编写。参赛队员负责确定需求、审查和修改生成代码、处理集成冲突、运行正确性/性能测试，并决定最终保留或撤销的实现。
- 优化思路曾参考 2020 年 USTC [FlammingMyCompiler](https://github.com/mlzeng/CSC2020-USTC-FlammingMyCompiler)、2021 年 [MahoShojo](https://github.com/wildoranges/MahoShojo)、2025 年剑桥大学[不队](https://gitlab.eduxiji.net/educg-group-36290-2935672/T202500000205464-2455)和电子科技大学 [0x676e616c63](https://gitlab.eduxiji.net/educg-group-36290-2935672/T202510614205710-2983)等公开项目，以及公开的经典编译原理和 RISC-V/AArch64 ABI/ISA 资料；本仓库未直接复制 GCC、LLVM 或上述项目源码。借鉴的是结构化循环优化、循环旋转、数据流优化、指令选择、向量化成本模型和图着色寄存器分配等通用思想，具体分析、数据结构、合法性条件和成本模型均结合本项目 IR 与后端独立实现。

## 验证记录

最新的有效、退化、未命中、不完整和违规实验分类见 [优化实验与消融记录](docs/optimization-experiment-log.md)。已删除的专项方案只保留在该文档的 E 类历史中，不属于当前编译器能力。

- Ubuntu 24.04/amd64 比赛容器内 Release 构建成功。
- `correctness` 通道最近一次完整回归为 140/140 AC。
- 当前 `wz` 的 `performance` 通道为 60/60 AC，`correctness` 通道为 140/140 AC；本地 QEMU 计时只用于版本间回归，不代表官方 BOOM 评测机时间。固定栈指针递归函数省略 frame pointer 后，官方 BOOM 上代表性递归热点从 `2,399,145,393` 降至 `2,143,867,371` cycles；继续加入受代码增长约束的递归自内联后降至 `1,261,469,485` cycles，相对前一提交再下降 `41.16%`，相对原始基线总下降 `47.42%`。
- 当前性能分析入口可记录计时区间动态指令、最热基本块及 RISC-V `generic-ooo` 近似调度结果；这些分析运行不参与计时。

当前 `BitwiseIdiomRecognitionPass` 识别二参数 i32 函数中的通用 radix-2 逐位累加：匹配器从自然循环、唯一预头、回边和 Phi 数据流出发，验证两个参数的 `%2`/`/2` 递推、常量 `2..32` 计数、单位位权和零累加器；随后对余数的 `00/01/10/11` 四种组合解释真实 CFG，只有累加器回边真值表严格等于 AND、OR 或 XOR 时才建立摘要。AND/OR 对负操作数按原有 `remainder == 1` 语义归零；XOR 对同号输入计算无符号 magnitude XOR、异号输入计算 magnitude OR，使用 i32 模取负覆盖 `INT_MIN`，从而对全部有符号输入删除原 32 轮 fallback。完整幂二乘法分派会把区间外 shift 安全选择为零并压缩成单块动态移位；晚期受预算内联随后消除这些 helper 调用，再运行全局标量本地化和 Mem2Reg，使跨调用状态只在入口加载、出口回写。匿名回归覆盖 5/32 位、正负数、`INT_MIN/MAX`、NAND 误匹配和越界 shift fallback。

上述 helper 闭式化、晚期内联和状态提升组合已在官方 Cortex-A53 上将 Huffman 三项由约 `13.92s` 降至 `7.67s`，下降约 `44.9%`、提速约 `1.81×`，三项均通过。

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

# 输出 AArch64 汇编（arm64 是等价别名）
/workspace/.docker-build/compiler input.sy --target aarch64 -o output-arm64.s

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
10. 所有地址不逃逸的整数标量局部量都参与栈对象提升，不再要求必须定义在入口块，因此循环体内的 `j/k/sum/val` 等热变量可以直接着色到寄存器；提升后释放对应帧空间。无调用、无有效栈对象、无栈参数且不使用 callee-saved 的叶函数完全省略栈帧及 `ra/s0` 保存恢复。含直接递归调用、函数体内不动态调整 `sp` 且不显式使用目标 frame-pointer 寄存器的函数改用固定 `sp` 作为帧基址，同时缩小保存区并删除 `s0` 保存、设置和恢复。

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

当前 `LoopUnrollAndJamPass` 对规范双层循环中的单位递增标量归约做 10 路展开合并。它要求归约结束后写回、无未知调用或内存写入，并证明同一全局对象在最内维上的跨 lane 访问互不别名；相邻 lane 对直接用于标量 load/store 的终端 GEP 复用 lane 0 地址，其余非终端、多维行地址仍按原语义独立计算。10 路上限来自 RV64 寄存器压力消融：12 路已在热循环产生栈回读且动态指令不再下降。该 pass 不读取函数名、全局名、数组大小或基本块名。以原 4 路版本为基线，`many_mat_cal-1` 在官方 50 MHz BOOM FPGA 上从 `1,367,087,082` 降到 `651,311,478` 周期，即 `27.3417s→13.0262s`，周期下降 `52.36%`、提速 `2.10x`，输出、返回值和完成标记保持正确。

当前 `AffineModularRecurrencePass` 会识别无副作用的零基单位计数循环，将 `state += loop_invariant_step` 以及带可证明安全保护的 `(state + step) % modulus` 递推改写为闭式计算。步长和模数可以是常量或任意循环不变量；动态带模路径会检查初值、次数、步长、模数和 i32 溢出条件，不满足时执行原循环。该 pass 可处理同一函数中的多个独立匹配，不读取符号名、数组尺寸或用例常数。官方 BOOM FPGA 上，`conv2d-1` 从 `5,514,638,911` 降到 `317,856,034` 周期，即 `110.2928s→6.3571s`、提速 `17.35x`；同一最终汇编下 `conv2d-2` 为 `86,417,335` 周期（`1.7283s`），`conv2d-3` 为 `30,274,169` 周期（`0.6055s`）。

当前 `DivRemPairStrengthReductionPass` 在同一基本块中复用已经计算的有符号二次幂余数。若先有 `r = x % ±2^k`、后有 `q = x / ±2^k`，则依据截断向零恒等式 `x = q*d + r`，用 `x-r` 和算术右移恢复商；负除数再取负。后端为合法常量移位直接生成 `slli/srli/srai` 及 word 版。该优化只使用 SSA 数据流、整数语义和 RISC-V ISA，不识别名称、固定尺寸或测试特征。60 项性能汇编仅 CRC 与 Huffman 各三项变化；官方 BOOM 上 `huffman-01` 从 `642,709,678` 降至 `494,988,287` cycles，下降 `22.98%`，输出、返回值和完成标记保持一致。

当前 `MonotonicContinueEliminationPass` 会识别从 0 开始、按 1 递增的规范 i32 循环。当循环在某个循环不变量阈值之后只执行无副作用的归纳变量自增与 `continue`，且归纳变量退出后不再使用时，第一次进入该单调后缀即可直接退出循环。该变换要求退出块无 Phi，并对有副作用 continue、非单位步长和退出值用途保守回退；不读取函数名、数组尺寸、阈值常数或测试结构。60 项汇编仅 `transpose0/1/2` 变化；官方 BOOM 上 `transpose2` 从 `913,978,753` 降至 `633,782,628` cycles，下降 `30.66%`，输出、返回值和完成标记保持一致。

当前 `RecursiveInliningPass` 对非 `void` 的直接自递归函数做受限展开。它按函数符号身份识别自调用，而不读取名字面量；在调用点完整克隆被调函数的 CFG、SSA 值、Phi 入边和返回汇合，并把克隆块紧邻调用块插入以保持现有后端的定义顺序。成本模型最多展开两层，且所有克隆体的静态指令预算不超过 224；无递归、`void` 返回、返回形态不完整或预计代码增长超限时保留原调用。结构化合成用例覆盖单递归、双递归副作用顺序、分支 Phi、局部数组、10 参数栈传递和 `void` fallback，结果 `6/6 AC`；60 项性能回归为 `60/60 AC`，最终汇编只改变 `knapsack_naive-1/2/3` 与 `fft0/1/2`。官方 BOOM 上，`knapsack_naive-1` 从 `2,143,867,371` 降至 `1,261,469,485` cycles，下降 `41.16%`；`fft0` 从 `11,135,817` 降至 `10,490,551` cycles，下降 `5.79%`。两项的输出、返回值和完成标记均保持正确。

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

AArch64 后端会把八路连续、独立的 `i32` 乘加归约合并为两组四路 NEON 运算。匹配器要求同一广播因子、连续加载、独立零初值、同序连续写回，以及清零块、循环回边和退出写回之间可验证的 CFG 关系；向量值存活区间内出现调用或寄存器额外使用时拒绝转换。该规则不读取源码、符号名、函数名、基本块名或数组尺寸。Cortex-A53 的 LLVM MCA 模型中，八路核心从 `8.0` 降至 `3.0 cycles`；ARM 功能测试 `140/140` 通过，60 项性能汇编中仅三项满足转换条件且均运行 AC。QEMU 对 NEON 软件模拟较慢，因此本地墙钟不作为该规则的 A53 成本判断。

中端会折叠跨嵌套循环的只读不变整数归约：当外层计数从零按一步递增、归约初值为零，并能证明循环体无调用、无外部写入、归约仅以 `acc = acc + delta` 更新且 `delta` 完全不依赖外层计数器时，只执行一次内层归约并按外层 trip count 缩放结果。局部槽必须在每次外层迭代中重新清零，计数器及其递增结果除循环控制外不得参与地址或数据计算；证明不完整即拒绝转换。该规则只读取 IR 的 CFG、use-def 和内存效果，不读取源码路径、用例名、函数名、符号名或固定数组尺寸。ARM 功能测试 `140/140` 通过，60 项性能汇编仅三项变化且输出一致；对应 QEMU 计时由约 30 秒降至约 1.89–2.01 秒。

AArch64 后端还会向量化连续的 `destination[i] = destination[i] * factor + source[i]` 整数循环。对指针参数不假设 `noalias`：进入四路 NEON 主循环前会用运行时范围检查证明源、目标区间不重叠，并证明广播因子不在目标写区；证明失败时完整保留原标量循环作为回退和尾循环。独立测试覆盖不别名、源/目标重叠和因子/目标重叠，均与逐标量实现一致。Cortex-A53 LLVM MCA 模型中，同样四个元素的核心吞吐由 `16.0` 降至 `4.5 cycles`；本地 QEMU A/B 从约 `4.62` 秒降至 `3.73` 秒。60 项性能汇编中仅三项满足该结构。

对于具有两个只读矩阵、一个独立输出矩阵和低位奇偶谓词的 `i-j-k` 整数归约，AArch64 后端会在证明唯一输出写入、静态维度一致且可四整除后交换为 `i-k-j`，保持每个输出元素原有的 `k` 累加顺序，并对连续的 `j` 维生成四路 NEON 掩码乘加。仿射布局消除在局部性收益打平时保留显式转置，避免过早破坏可向量化布局。Cortex-A53 模型中，四个标量贡献约为 `30.0 cycles`，四路内核为 `6.5 cycles`；ARM64 原生缓存机器交错 A/B 约为 `1.96–2.01` 秒对 `0.1246–0.1248` 秒。QEMU 因不反映真实跨行缓存代价而显示反向结果（约 `4.42` 秒对 `6.10` 秒），因此不用于该变换的接受判断。

AArch64 后端会折叠正数尾循环中的连续除二步骤：当 CFG 能证明循环头先检查状态是否为一，随后按最低位分流，偶数路径仅执行有符号除二、计数器加一并回到同一循环头时，使用 `rbit + clz` 计算尾零数，一次完成多次逻辑右移和计数累加。运行时正数检查保证零与负数仍执行原有单步路径。该匹配只读取汇编数据流和 CFG，不读取源码、函数名或用例名。60 项性能汇编仅三项命中，本地三项合计约由 `39.84` 秒降至 `38.57` 秒；匿名正负数对拍一致，ARM 功能回归 `140/140 AC`。

在上述尾循环同时被证明为无调用、无写内存、仅读取一个不变上下文的二参数纯函数，且第二参数只作为加法归约计数器时，AArch64 后端会为状态到“剩余计数增量”建立有界稠密摘要。缓存项同时记录上下文，因而全局边界改变后不会错误复用；只有到达加法返回路径的调用才写入，常量返回路径不缓存。状态窗口固定为 `2^20`，每个模块至多转换一个函数，以约束 A53 随机访存成本和 BSS 预算。匹配只依赖汇编 CFG、寄存器数据流与内存效果，不读取源码路径、函数名、用例名、固定输入或输出。匿名回归覆盖不同初始计数、上下文切换、窗口外状态和常量返回；ARM 功能回归 `140/140 AC`，60 项性能汇编仅三项变化。相对连续除二基线，三项 QEMU 计时分别由 `24.2865/2.1766/12.1084s` 降至 `7.1480/0.4588/3.2137s`。

中端会把连续整数分派表示的幂二运算归一化为范围保护后的变量移位：当二参数 i32 函数完整枚举连续的 `n == 1..K`，每条真分支仅返回 `x * 2^n` 或 `x / 2^n`，范围外返回 `x`，且 CFG 中没有额外块或操作时，乘法分派直接使用 `shl`；有符号除法仅在运行时证明 `x >= 0` 后使用 `lshr`，负数及范围外输入保留原分派 CFG。匹配读取 IR 类型、CFG 和 SSA 数据流，不读取函数名、源码路径、用例或输入。匿名 O0/O1 对拍覆盖负数、i32 回绕和范围外参数；ARM 正确性 `140/140 AC`，60 项仅三项 Huffman 汇编变化，QEMU 合计由 `35.1920s` 降至 `31.8483s`，下降约 `9.5%`。

中端还会对规范单位递增循环做带仿射不别名证明的标量提升。候选地址必须在循环外定义，循环内只能有一次对该地址的 load 和一次条件 store，且无调用或其他 store；同一数组对象的其余 load 必须通过完整 GEP 下标、循环条件和归纳变量初值证明至少一维恒不相等。变换用运行时入口条件保留零趟循环，在入口加载一次，通过 Phi 在寄存器中维护当前值，删除循环内目标 load/store，并在唯一出口一次写回。更新块中的重复 load 会由支配它的循环主体 load 转发；仅含可安全推测整数运算时进一步改成 `select`。AArch64 MIR 会把单用途 `icmp + select` 合并为直接的 `cmp + csel`，避免布尔值物化。合成回归覆盖零趟、单趟、多趟、无更新和无法证明别名时的 fallback；ARM performance `60/60 AC`、correctness `140/140 AC`。60 项中 `h-8` 三项命中标量提升，`h-4` 与 transpose 各三项仅命中通用 compare-select 合并；真实 Cortex-A53 收益仍需官方平台复核。

AArch64 优化链会进一步四路展开严格有序的只读整数 `select` 递推。匹配要求单位递增、唯一预头和回边、一个携带 current 的递推 Phi、条件依赖旧 current，且循环体没有 store、call、额外 Phi、浮点或向量副作用；运行时边界守卫保留零趟和 0–3 次标量尾部，四个 lane 严格按原顺序执行。步长四的跨行地址同时转成最多四个固定步长 pointer Phi，热体不再计算动态 stride 乘法。规则只使用 CFG、SSA 和内存效果，不读取任何名称、尺寸或输入。匿名测试覆盖 i32 回绕和含 store fallback；AArch64 correctness `140/140 AC`，60 份性能汇编仅三份变化，本地 QEMU 完整输入约改善 `10%–13%`，真实收益仍由 Cortex-A53 复核。

## 当前限制

- 仅支持单输入文件。
- 类型检查和语义分析仍有部分语言边界未完整覆盖。
- IR 尚未形成完整 use-def 链，优化框架仍在演进。
- 当前维护完整的 `riscv64` 与 `aarch64` 后端链路；AArch64 已支持受严格 CFG 与数据流门槛约束的整数乘加 SLP，尚未形成通用循环向量化框架。
- 当前比赛链接口径为 `rv64gc`，不包含 RISC-V Vector Extension，因此不能直接生成 RVV 指令；可继续采用标量循环展开、软件流水、访存合并和指令调度。
- 本地性能评测依赖 QEMU，数值与官方硬件不可直接比较；`generic-ooo` 同样只是乱序流水线近似模型，不模拟官方 BOOM 的缓存和内存系统。
