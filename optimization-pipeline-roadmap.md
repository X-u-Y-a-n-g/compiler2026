# Optimization Pipeline Roadmap

本文档给我们的编译器设计一条完整的代码优化建设路线。目标不是只把 pass 数量堆长，而是形成一套可以逐阶段实现、测试、回归和调参的体系：

```text
基础设施
  -> IR 规范化
  -> SSA / Mem2Reg
  -> 函数内联
  -> 标量优化
  -> 内存优化
  -> 循环优化
  -> 后内联
  -> 最终收尾
  -> MIR / 后端优化
  -> 比赛调优
```

设计参考了几个 EduXiji 仓库的共同点：

- 优化不是单次线性 pass，而是大量 `clean -> expose -> clean` 循环。
- `Mem2Reg` 通常作为主优化入口，后续多次清理和再 SSA 化会吃掉新暴露的机会。
- 函数内联不应只跑一次，应该分 early inline 和 late inline。
- 循环优化前需要循环规范化，循环优化后必须再跑清理。
- 后端也需要独立流水线，不应该只做寄存器分配和栈帧生成。

## 当前状态

当前 `-O1` 中端流水线在 `src/driver/Driver.cpp` 中直接手写：

```text
Mem2Reg
FunctionInlining
CSE
CSE
RepeatedAccumulationElimination
DCE
TrivialBlockElimination
```

当前已存在但没有完整接入体系的 pass：

| Pass | 状态 | 说明 |
| --- | --- | --- |
| `Mem2Reg` | 已接入 | 当前 O1 开头执行 |
| `FunctionInlining` | 已接入 | 目前偏保守，只适合小型纯函数 |
| `CommonSubexpressionElimination` | 已接入 | 当前重复跑两次，但没有 fixed-point 管理 |
| `RepeatedAccumulationElimination` | 已接入 | 面向特定循环形态 |
| `DeadCodeElimination` | 已接入 | 已增强函数纯度、死 phi、死函数删除 |
| `TrivialBlockElimination` | 已接入 | 目前承担一部分 CFG 清理 |
| `ConstantFolding` | 已实现，未接入 | 应进入基础清理和收尾 |
| `LocalScalarCleanup` | 已实现，未接入 | 应进入标量清理循环 |
| `LoopInvariantCodeMotion` | 已实现，未接入 | 应进入循环优化阶段 |

当前后端流水线：

```text
IR -> MIR lowering
PhiElimination
StackObjectPromotion
RegisterAllocator
FrameLowering
StackLowering
AsmPrinter
```

主要问题：

- Driver 中直接拼 pass，缺少 PipelineBuilder。
- pass 不返回 `changed`，无法可靠 fixed-point。
- 缺少 AnalysisManager，DomTree、LoopInfo、AliasInfo 等会在各 pass 内重复构建。
- 缺少标准 clean pipeline。
- 缺少 SCCP、GVN、DSE、LoadElim、LoopSimplify、LoopUnroll、CodeGenPrepare 等关键 pass。
- 后端优化几乎为空。

## 最终目标流水线

最终希望 `-O1` 形成如下完整流水线：

```text
0. Frontend lowering
1. IR canonicalization
2. SSA enabling
3. Early inline loop
4. Scalar simplification loop
5. Memory optimization loop
6. Loop optimization pipeline
7. Late inline pipeline
8. Final cleanup and codegen prepare
9. MIR optimization pipeline
10. Assembly emission
```

详细展开：

```text
IRCanonicalize
  CFGSimplify
  ConstantFolding
  LocalScalarCleanup
  DCE

SSAEnabling
  Mem2Reg
  CFGSimplify
  ConstantFolding
  CSE/GVN
  DCE

EarlyInlineLoop, fixed-point max 3
  FunctionInlining
  Mem2Reg
  ScalarClean
  CFGSimplify

ScalarMainLoop, fixed-point max 4
  InstSimplify
  ConstantFolding
  SCCP
  Reassociate
  CSE/GVN
  DCE
  CFGSimplify

MemoryMainLoop, fixed-point max 3
  AliasAnalysis
  LoadElimination
  DeadStoreElimination
  StoreToLoadForwarding
  Mem2Reg
  DCE

LoopPipeline
  LoopSimplify
  LCSSA
  LICM
  LoopRotate
  LICM
  IndVarSimplify
  LoopStrengthReduce
  RepeatedAccumulationElimination
  LoopElimination
  LoopUnroll
  ScalarClean
  MemoryClean

LateInlinePipeline, fixed-point max 2
  LateFunctionInlining
  Mem2Reg
  ScalarClean
  DCE

FinalCleanup
  ADCE
  CFGSimplify
  ConstantFolding
  CSE/GVN
  DCE
  CodeGenPrepare

MIRPipeline
  ISel / Lowering
  MachineConstantFold
  CopyPropagation
  MachineCSE
  MachineDCE
  PeepholeBeforeRA
  PhiElimination
  StackObjectPromotion
  RegisterAllocator
  PeepholeAfterRA
  FrameLowering
  StackLowering
  BranchSimplify
  BlockLayout
  AsmPrinter
```

## 阶段 0: Pass 基础设施

目标：先让流水线能表达复杂结构。

### 要完成

1. 新增 `MidendPipelineBuilder`。
2. 将 driver 中的 pass 拼接逻辑挪出去。
3. `ModulePass::run` 返回 `bool changed`。
4. `PassManager::run` 返回整体是否 changed。
5. 支持 `FixedPointPassManager`。
6. 支持组合 pipeline：
   - `buildCleanupPipeline`
   - `buildScalarPipeline`
   - `buildMemoryPipeline`
   - `buildLoopPipeline`
   - `buildO1Pipeline`
7. 每个 pass 记录执行次数、是否改变 IR。
8. 每轮 fixed-point 设置最大轮数，避免死循环。

### 建议接口

```cpp
class ModulePass {
 public:
  virtual ~ModulePass() = default;
  virtual std::string name() const = 0;
  virtual bool run(Module& module) = 0;
};

class PassManager {
 public:
  bool run(Module& module);
  bool runFixedPoint(Module& module, int maxRounds);
};
```

### 验收标准

- 原有 O1 功能测试全部通过。
- 能在 dump IR 中看到 pass 顺序。
- 能打印每个 pass 是否改变 IR。
- fixed-point 不会无限循环。

## 阶段 1: 基础清理体系

目标：形成 `clean pipeline`，给所有后续优化复用。

### 要接入已有 pass

```text
ConstantFolding
LocalScalarCleanup
DCE
TrivialBlockElimination
CSE
```

### 要新增 / 扩展

| Pass | 说明 |
| --- | --- |
| `CFGSimplify` | 从 `TrivialBlockElimination` 扩展，处理空块、单跳转块、不可达块、常量条件分支 |
| `InstSimplify` | 代数恒等式、比较恒等式、无效转换清理 |
| `PhiSimplify` | 简化单一来源 phi、删除死 phi 环 |

### 阶段流水线

```text
BasicClean, fixed-point max 4
  ConstantFolding
  InstSimplify
  LocalScalarCleanup
  CSE
  DCE
  CFGSimplify
  PhiSimplify
```

### 验收标准

- 每个优化 pass 后跑 IR verifier。
- functional `-O0` 不受影响。
- performance `-O1` 不降正确率。
- 清理 pipeline 可以在任意阶段重复调用。

## 阶段 2: SSA 和 Mem2Reg 体系

目标：让后续标量优化建立在稳定 SSA 之上。

### 要完成

1. 整理 `Mem2Reg` 的边界：
   - 只提升标量 alloca。
   - 支持入口块 alloca。
   - phi 插入、重命名、undef 处理可靠。
2. 增加 `PromoteAfterInline` 场景：
   - inline 后新暴露的 alloca/load/store 要能再次提升。
3. 增加 use-def 辅助工具。
4. 增加 DominatorTree 分析。

### 阶段流水线

```text
SSAEnabling
  CFGSimplify
  Mem2Reg
  PhiSimplify
  BasicClean
```

### 验收标准

- mem2reg 单测覆盖 if、while、多前驱、多后继、嵌套循环。
- inline 后再次 mem2reg 不破坏 IR。
- phi 的 incoming block 与 CFG 始终一致。

## 阶段 3: 函数内联体系

目标：把函数内联拆成 early inline 和 late inline。

### Early Inline

适合阶段：

```text
Mem2Reg 之后，标量优化之前
```

策略：

- 内联小函数。
- 优先内联纯函数。
- 优先内联只被调用一次的函数。
- 限制递归。
- 限制代码膨胀。

流水线：

```text
EarlyInlineLoop, fixed-point max 3
  FunctionInlining
  Mem2Reg
  BasicClean
```

### Late Inline

适合阶段：

```text
主要标量/循环优化之后，最终收尾之前
```

策略：

- 支持多基本块函数。
- 支持多个 return，用 phi 汇合返回值。
- 支持 void 函数。
- 支持简单副作用函数，但必须保持调用顺序。
- 对热点调用点放宽阈值。

流水线：

```text
LateInlineLoop, fixed-point max 2
  LateFunctionInlining
  Mem2Reg
  BasicClean
```

### 要新增

| 组件 | 说明 |
| --- | --- |
| `CallGraphAnalysis` | 建函数调用图 |
| `FunctionPurityAnalysis` | 区分 pure、readonly、impure |
| `InlineCostModel` | 根据指令数、调用次数、循环深度估算收益 |
| `LateFunctionInliningPass` | 多基本块 inline |

### 验收标准

- functional 全过。
- 内联递归函数不会死循环。
- 多 return 函数 inline 后 CFG 正确。
- 内联后死函数能被 DCE 删除。

## 阶段 4: 标量优化主循环

目标：将普通算术、比较、常量传播、公共表达式消除形成体系。

### 要新增

| Pass | 说明 |
| --- | --- |
| `SCCP` | Sparse Conditional Constant Propagation |
| `Reassociate` | 重结合，暴露 CSE/GVN 和 LICM 机会 |
| `GVN` | 逐步替代局部 CSE |
| `RangeAnalysis` | 支持比较简化、循环边界推断 |
| `CompareSimplify` | 比较链、恒真恒假分支折叠 |

### 阶段流水线

```text
ScalarMainLoop, fixed-point max 4
  InstSimplify
  ConstantFolding
  SCCP
  Reassociate
  CSE
  GVN
  CompareSimplify
  DCE
  CFGSimplify
```

### 实现顺序

1. `InstSimplify`
2. `SCCP`
3. `Reassociate`
4. 将当前 CSE 升级为局部 GVN。
5. 再实现跨基本块 GVN。

### 验收标准

- 常量分支能触发 CFG 清理。
- 算术链能被重结合后 CSE/GVN。
- 不因为浮点重结合破坏语义；初期只对整数做 agressive reassociate。

## 阶段 5: 内存优化体系

目标：吃掉 SysY 性能样例里大量数组、局部变量、load/store 冗余。

### 要新增

| Pass | 说明 |
| --- | --- |
| `BasicAliasAnalysis` | 保守别名分析 |
| `LoadElimination` | 消除重复 load |
| `DeadStoreElimination` | 删除被覆盖且不再读取的 store |
| `StoreToLoadForwarding` | `store x -> load same_addr` 直接替换 |
| `ScalarReplacement` | 简单聚合拆成标量，后续交给 Mem2Reg |

### 阶段流水线

```text
MemoryMainLoop, fixed-point max 3
  BasicAliasAnalysis
  LoadElimination
  StoreToLoadForwarding
  DeadStoreElimination
  ScalarReplacement
  Mem2Reg
  BasicClean
```

### 实现顺序

1. 同一基本块内 `LoadElimination`。
2. 同一基本块内 `DSE`。
3. 基于 DominatorTree 的跨块 load/store 优化。
4. 引入保守 AliasAnalysis。
5. 简单局部数组 scalar replacement。

### 验收标准

- 有调用、未知指针、数组写入时保持保守。
- 不跨越 impure call 消除 load/store。
- memory pass 后 IR verifier 全过。

## 阶段 6: 循环优化体系

目标：把性能赛主要收益集中到循环。

### 要新增分析

| Analysis | 说明 |
| --- | --- |
| `LoopAnalysis` | 自然循环识别 |
| `LoopSimplify` | preheader、单 latch、专用 exit |
| `LCSSA` | 循环外使用通过 phi 汇合 |
| `SCEV` | 归纳变量和 trip count 分析 |
| `LoopAliasAnalysis` | 循环内数组访问分析 |

### 要实现 / 增强 pass

| Pass | 说明 |
| --- | --- |
| `LICM` | 已有，增强 alias/call 安全性 |
| `LoopRotate` | 转成更适合 LICM 和 unroll 的形态 |
| `IndVarSimplify` | 简化归纳变量 |
| `LoopStrengthReduce` | 乘法索引改加法递推 |
| `LoopElimination` | 删除无副作用空循环/可计算循环 |
| `LoopUnroll` | 常量 trip count 全展开/部分展开 |
| `LoopUnswitch` | 条件与循环无关时外提 |
| `ArrayAccessSimplify` | 数组地址计算简化 |

### 阶段流水线

```text
LoopPipeline
  LoopSimplify
  LCSSA
  LICM
  BasicClean

  LoopRotate
  LoopSimplify
  LCSSA
  LICM
  BasicClean

  IndVarSimplify
  LoopStrengthReduce
  RepeatedAccumulationElimination
  LoopElimination
  BasicClean

  LoopUnroll
  BasicClean
  MemoryMainLoop
```

### 实现顺序

1. `LoopAnalysis`
2. `LoopSimplify`
3. 接入并修正现有 `LICM`
4. `IndVarSimplify`
5. `LoopStrengthReduce`
6. `LoopUnroll`
7. `LoopElimination`
8. `LoopUnswitch`

### 验收标准

- 嵌套循环 CFG 正确。
- LICM 不外提可能异常或有副作用的指令。
- LoopUnroll 有代码体积阈值。
- 性能样例中矩阵乘、卷积、FFT、排序类有可观察收益。

## 阶段 7: 后内联和模块级优化

目标：在大量优化后，用 late inline 再打开一批跨函数机会。

### 要新增

| Pass | 说明 |
| --- | --- |
| `TreeShaking` | 删除不可达函数、未使用声明 |
| `GlobalDCE` | 删除无用全局 |
| `Internalize` | 非入口函数内部化 |
| `GlobalConstProp` | 全局常量传播 |
| `FunctionSpecialization` | 针对常量实参克隆函数 |

### 阶段流水线

```text
ModuleOpt
  Internalize
  TreeShaking
  GlobalConstProp
  FunctionSpecialization
  LateInlineLoop
  TreeShaking
  BasicClean
```

### 验收标准

- `main`、运行时函数、外部声明不会被误删。
- 函数克隆有数量限制。
- late inline 后能触发 scalar/memory clean。

## 阶段 8: 最终收尾和 CodeGenPrepare

目标：给后端一个更容易 lowering 的 IR。

### 要新增

| Pass | 说明 |
| --- | --- |
| `ADCE` | 控制依赖驱动的激进死代码删除 |
| `SelectSimplify` | 简化 select/条件表达式 |
| `CodeGenPrepare` | 降低后端复杂度 |
| `LowerIntrinsics` | 内建函数或高级 IR 指令下降 |
| `NameNormalize` | 稳定 dump，便于 diff |

### 阶段流水线

```text
FinalCleanup
  ADCE
  CFGSimplify
  ConstantFolding
  SCCP
  GVN
  DCE
  PhiSimplify
  CodeGenPrepare
  LowerIntrinsics
  NameNormalize
```

### 验收标准

- 后端不再处理明显冗余 IR。
- dump IR 稳定，便于回归测试。
- `CodeGenPrepare` 不做高层语义优化，只做 lowering 友好化。

## 阶段 9: MIR / 后端优化体系

目标：后端不能只完成翻译，还要做机器级清理和调度。

### 要新增 pass

| Pass | 说明 |
| --- | --- |
| `MachineConstantFold` | 机器指令常量折叠 |
| `CopyPropagation` | 消除多余 move/copy |
| `MachineDCE` | 删除无用机器指令 |
| `MachineCSE` | 机器级公共表达式 |
| `PeepholeBeforeRA` | RA 前窥孔 |
| `PeepholeAfterRA` | RA 后窥孔 |
| `BranchSimplify` | 跳转链、空块、反转分支 |
| `BlockLayout` | 基本块布局优化 |
| `MachineLICM` | 机器级循环不变量外提 |
| `PostRAScheduler` | RA 后局部调度 |

### 后端目标流水线

```text
MIRPipeline
  InstructionSelection
  MachineConstantFold
  CopyPropagation
  MachineCSE
  MachineDCE
  BranchSimplify
  PeepholeBeforeRA

  PhiElimination
  StackObjectPromotion
  MachineLICM
  RegisterAllocator

  PeepholeAfterRA
  FrameLowering
  StackLowering
  BranchSimplify
  BlockLayout
  PostRAScheduler
  AsmPrinter
```

### 实现顺序

1. `MachineDCE`
2. `CopyPropagation`
3. `PeepholeBeforeRA`
4. `PeepholeAfterRA`
5. `BranchSimplify`
6. `BlockLayout`
7. `MachineLICM`
8. `PostRAScheduler`

### 验收标准

- 后端 pass 不破坏寄存器分配约束。
- RA 后 pass 不引入新的虚拟寄存器。
- 输出汇编能通过功能和性能评测。

## 阶段 10: 调参、评测和回归体系

目标：优化 pass 必须可控、可回退、可做消融实验。

### 要完成

1. 支持单 pass 开关：
   - `--disable-pass=dce`
   - `--enable-pass=loop-unroll`
2. 支持 dump 指定 pass 前后 IR：
   - `--dump-before=licm`
   - `--dump-after=licm`
3. 支持 pass 统计：
   - 删除指令数
   - 删除基本块数
   - 内联次数
   - unroll 次数
   - load/store 消除次数
4. 建立性能消融脚本：
   - 全开
   - 关闭 inline
   - 关闭 loop
   - 关闭 memory
   - 关闭 backend peephole
5. functional 使用 `-O0`。
6. performance 使用 `-O1`。

### 验收标准

- 每次新增 pass 都能跑：
  ```text
  functional -O0
  performance -O1
  targeted regression
  ```
- 任何性能提升必须附带正确性结果。
- 任何 pass 都可以单独关闭定位问题。

## 建议里程碑

| 阶段 | 名称 | 完成后效果 |
| --- | --- | --- |
| 0 | Pass 基础设施 | 流水线可组合、可 fixed-point |
| 1 | 基础清理 | 所有阶段都有统一 clean |
| 2 | SSA 体系 | Mem2Reg 成为稳定优化入口 |
| 3 | 函数内联 | early/late inline 打开跨函数优化 |
| 4 | 标量优化 | 常量、表达式、分支优化成体系 |
| 5 | 内存优化 | load/store 冗余开始大量减少 |
| 6 | 循环优化 | 性能赛主要热点开始受益 |
| 7 | 模块优化 | 死函数、全局常量、函数特化 |
| 8 | 最终收尾 | IR 更适合后端 |
| 9 | 后端优化 | 汇编质量明显提升 |
| 10 | 调参与回归 | 优化体系可维护、可定位 |

## 最终 O1 Pipeline 草案

当全部阶段完成后，`-O1` 可以按如下结构构建：

```text
buildO1Pipeline:
  IRCleanInitial
  SSAEnabling

  EarlyInlineLoop
  ScalarMainLoop
  MemoryMainLoop

  LoopPipeline
  ScalarMainLoop
  MemoryMainLoop

  ModuleOpt
  LateInlineLoop

  FinalCleanup
  CodeGenPrepare

  MIRPipeline
```

进一步展开：

```text
CFGSimplify
ConstantFolding
LocalScalarCleanup
DCE

Mem2Reg
PhiSimplify
BasicClean

repeat 3:
  FunctionInlining
  Mem2Reg
  BasicClean

repeat 4:
  InstSimplify
  ConstantFolding
  SCCP
  Reassociate
  CSE/GVN
  DCE
  CFGSimplify

repeat 3:
  LoadElimination
  StoreToLoadForwarding
  DSE
  ScalarReplacement
  Mem2Reg
  BasicClean

LoopSimplify
LCSSA
LICM
LoopRotate
LCSSA
LICM
IndVarSimplify
LoopStrengthReduce
RepeatedAccumulationElimination
LoopElimination
LoopUnroll
BasicClean
MemoryMainLoop

TreeShaking
GlobalConstProp
FunctionSpecialization

repeat 2:
  LateFunctionInlining
  Mem2Reg
  BasicClean

ADCE
CFGSimplify
SCCP
GVN
DCE
CodeGenPrepare
LowerIntrinsics
NameNormalize

InstructionSelection
MachineConstantFold
CopyPropagation
MachineCSE
MachineDCE
PeepholeBeforeRA
PhiElimination
StackObjectPromotion
MachineLICM
RegisterAllocator
PeepholeAfterRA
FrameLowering
StackLowering
BranchSimplify
BlockLayout
PostRAScheduler
AsmPrinter
```

## 重要原则

1. 每个复杂 pass 后面必须跟 clean。
2. Inline、LoopUnroll、SCCP、DSE 这类 pass 会暴露大量机会，后面必须接 `BasicClean`。
3. 循环优化前必须有 `LoopSimplify`，否则 LICM、LSR、Unroll 会越来越难写。
4. 内存优化必须保守，先保证正确，再逐步放宽。
5. 后端优化分 RA 前和 RA 后，RA 后不能随便引入虚拟寄存器。
6. `-O0` 服务 functional 正确性，`-O1` 服务 performance。
7. 每个 pass 都要能单独关闭，方便定位错误。
8. 每个阶段完成后都要做消融评测，确认收益来自哪里。

