# 2026 编译系统实现赛 SysY 编译器

本项目实现一个面向 2026 全国大学生计算机系统能力大赛编译系统实现赛 RISC-V 赛道的 SysY 编译器。当前代码基线为 `de1ac8e FunctionInlining`。

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
- `-O1`：依次运行 Mem2Reg、函数内联、两轮公共子表达式消除、重复累加消除、死代码消除和空基本块消除。
- 后端：支持 RISC-V 64 指令选择、Phi 消除、栈对象提升、图着色寄存器分配、栈帧与汇编输出。
- 计时函数：`starttime()` / `stoptime()` 会被降级为带源码行号的 `_sysy_starttime(int)` / `_sysy_stoptime(int)` 调用。

当前版本已验证：

- Ubuntu 24.04/amd64 比赛容器内 Release 构建成功。
- `correctness` 通道 140/140 AC：100 个 `functional` 和 40 个 `h_functional` 全部通过链接、QEMU 执行和输出对拍。
- `performance` 通道 60/60 AC，全部记录了逐用例内部计时。
- 同一容器、同一批用例的评测中，循环局部标量提升版本内部计时总和为 `10.105372s`；无栈叶函数帧消除版本为 `10.363939s`，本轮降低约 `2.49%`；直接寄存器发射版本为 `10.581239s`，累计降低约 `4.50%`；旧线性扫描版本为 `12.375371s`，累计降低约 `18.34%`。其中 `many_mat_cal-1` 从 `0.452314s` 降至 `0.369974s`，降低约 `18.20%`。
- 超大压力用例 `h_functional/29_long_line.sy` 的编译时间约为 `0.13s`，不会因稠密冲突图超时。
- 性能样例可输出官方格式的 `Timer@...` 和 `TOTAL: ...` 计时信息。

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

- `results.tsv`：逐用例状态及 `compile_sec`、`link_sec`、`run_wall_sec`、`timer_sec`、`total_wall_sec`。
- `summary.txt`：适合终端查看的汇总。
- `summary.json`：机器可读汇总。
- `failures/`：失败用例的源码、stdout、stderr、期望输出和实际输出。
- `runtime/sylib.c`：本次评测使用的公开 SysY 运行时副本。

注意：QEMU 时间只适合本地回归和不同提交之间的粗略比较，不能替代官方 FPGA BOOM CPU 开发板成绩。平台上一次 AC 记录中的 `1582.7551s` 是 60 个 2026 性能用例在官方开发板上的计时总和。

## 当前限制

- 仅支持单输入文件。
- 类型检查和语义分析仍有部分语言边界未完整覆盖。
- IR 尚未形成完整 use-def 链，优化框架仍在演进。
- 当前只维护完整的 `riscv64` 后端链路。
- 本地性能评测依赖 QEMU，数值与官方硬件不可直接比较。
