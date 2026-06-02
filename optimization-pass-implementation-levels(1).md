# Optimization Pass Implementation Levels

本文档把优化路线拆成“每个 pass 要做到什么程度”。形式参考课程评分标准：每个 pass 分为若干实现层级，先保证低层级正确，再逐步提升到完整版本。

层级约定：

| 等级 | 含义 |
| --- | --- |
| L0 | 骨架完成，接入 pipeline，可开关，可 dump，不要求真实优化 |
| L1 | 局部/简单场景可用，适合先拿正确性 |
| L2 | 跨基本块或常见控制流可用，开始产生稳定收益 |
| L3 | 完整常规实现，能覆盖大部分性能样例 |
| L4 | 增强实现，包含复杂形态、调参、统计和消融 |

## 0. 通用基础设施

| 项目 | L1 | L2 | L3 | L4 |
| --- | --- | --- | --- | --- |
| `PassManager` | pass 可顺序运行 | pass 返回 `changed` | fixed-point pipeline | pass 开关、统计、before/after dump |
| `PipelineBuilder` | O0/O1 分离 | clean/scalar/loop 子 pipeline | early/late inline 分层 | 支持消融配置 |
| `IRVerifier` | 检查基本块终结指令 | 检查 phi incoming 与 CFG | 检查 def-use/类型/可达性 | 每个 pass 后自动验证 |
| `AnalysisManager` | 手工获取分析 | 缓存 DomTree/CFG | changed 后失效分析 | 分析依赖自动管理 |

验收：

```text
任意 pass 能单独打开/关闭
任意 pipeline 能 fixed-point
每个 pass 能报告 changed
每个 pass 后可跑 verifier
```

## 1. CFGSimplify / TrivialBlockElimination

| 等级 | 实现范围 |
| --- | --- |
| L1 | 删除不可达基本块；删除不可达指令；删除空跳转块 |
| L2 | 合并单前驱单后继基本块；更新 phi incoming |
| L3 | 常量条件分支改无条件跳转；删除无用分支边；清理死 phi |
| L4 | 跳转链压缩；反转分支减少跳转；循环 preheader/exit 形态保持 |

依赖：

```text
CFG 构建
phi incoming 更新工具
IRVerifier
```

验收样例：

```text
if (0) dead;
while (0) dead;
空块跳转链
删除前驱后的 phi 修正
```

## 2. DCE / ADCE

| 等级 | 实现范围 |
| --- | --- |
| L1 | 删除没有 use 且无副作用的普通指令 |
| L2 | 删除死 phi；删除死 phi 环；删除死 basic block 内指令 |
| L3 | 基于函数纯度删除无用 pure call；删除不可达函数 |
| L4 | ADCE：从 return/store/call/br 等根集合反向标记，删除控制依赖死代码 |

依赖：

```text
use-def 或全函数 use 统计
FunctionPurityAnalysis
CFG 可达性
```

验收：

```text
无 use 算术指令被删除
死 phi 环被删除
pure 函数调用结果不用时被删除
impure call / store / return 保留
```

## 3. Mem2Reg

| 等级 | 实现范围 |
| --- | --- |
| L1 | 构建 CFG；删除不可达指令和不可达基本块 |
| L2 | 删除没有 use 的 def 变量及相关指令 |
| L3 | def/use 在同一基本块时的局部 mem2reg |
| L4 | 构建支配树；根据支配边界插入 phi；变量重命名；完整 mem2reg |

进一步增强：

| 等级 | 实现范围 |
| --- | --- |
| L4+ | pruned phi：只在 live-in 块插入 phi |
| L4++ | inline 后再次 mem2reg；支持 undef；支持复杂循环 |

依赖：

```text
CFG
DominatorTree
DominanceFrontier
phi 指令
def-use / rename stack
```

验收：

```text
单块 store/load 消除
if-else 合流插入 phi
while 循环 header 插入 phi
嵌套循环变量正确重命名
删除 alloca/load/store 后 IR verifier 通过
```

## 4. ConstantFolding / InstSimplify

| 等级 | 实现范围 |
| --- | --- |
| L1 | 整数二元运算常量折叠 |
| L2 | 比较、类型转换、布尔运算折叠 |
| L3 | 分支条件常量化；代数恒等式：`x+0`、`x*1`、`x*0` |
| L4 | 浮点安全折叠；多指令局部常量传播；配合 SCCP |

依赖：

```text
常量解析/格式化工具
类型系统
CFG simplify
```

验收：

```text
1 + 2 -> 3
x * 1 -> x
x * 0 -> 0
if (1 < 2) -> 无条件跳转
```

## 5. LocalScalarCleanup

| 等级 | 实现范围 |
| --- | --- |
| L1 | 单基本块内 copy/alias 替换 |
| L2 | 删除冗余临时变量 |
| L3 | 结合 ConstantFolding 做局部值传播 |
| L4 | 跨简单直线块传播，遇到分支/副作用保守停止 |

依赖：

```text
局部 use-def
副作用判断
```

验收：

```text
%a = add x, 0
%b = add %a, 1
替换为直接使用 x
```

## 6. CSE / GVN

| 等级 | 实现范围 |
| --- | --- |
| L1 | 同一基本块内 CSE |
| L2 | 支配关系下跨基本块 CSE |
| L3 | GVN：为等价表达式编号，支持 commutative 运算 |
| L4 | GVN-PRE：部分冗余消除，处理跨分支重复表达式 |

依赖：

```text
DominatorTree
表达式 canonicalize
AliasAnalysis：处理 load 时需要
```

验收：

```text
同块重复 add 删除
if 前计算的表达式在 then/else 中复用
a+b 与 b+a 视为同一表达式
load 只有在安全时才 CSE
```

## 7. SCCP

| 等级 | 实现范围 |
| --- | --- |
| L1 | 普通常量传播 lattice：unknown/constant/overdefined |
| L2 | 稀疏传播 SSA value；替换常量 value |
| L3 | 同时传播可达基本块；折叠条件分支 |
| L4 | 支持 phi、循环、函数参数保守传播；与 CFGSimplify 联动 |

依赖：

```text
SSA
CFG
worklist
ConstantFolding
```

验收：

```text
常量参数一路传播
不可达分支被删除
phi 全常量时被折叠
循环中不确定值标记 overdefined
```

## 8. Reassociate

| 等级 | 实现范围 |
| --- | --- |
| L1 | 整数加法/乘法树扁平化 |
| L2 | 常量聚合：`(x+1)+2 -> x+3` |
| L3 | 操作数排序，暴露 CSE/GVN |
| L4 | 提取公共因子，辅助 LICM/LSR |

依赖：

```text
整数表达式树识别
无溢出/语义策略
```

验收：

```text
(a+b)+a 与 a+(a+b) 规范到一致形态
i*4 + j*4 -> (i+j)*4 或暴露公共因子
```

## 9. FunctionInlining / LateFunctionInlining

| 等级 | 实现范围 |
| --- | --- |
| L1 | 单基本块、小型、纯函数内联 |
| L2 | void 函数、简单返回值函数内联 |
| L3 | 多基本块函数内联；调用点拆块；多 return 用 phi 汇合 |
| L4 | 成本模型；递归限制；热点循环内调用提高内联优先级；inline 后删死函数 |

依赖：

```text
CallGraphAnalysis
FunctionPurityAnalysis
CFG split/clone 工具
fresh name 生成
Mem2Reg
DCE
```

验收：

```text
小 pure 函数被 early inline
多 return 函数 late inline 后结果正确
递归函数不会无限内联
inline 后 dead callee 被删除
```

## 10. FunctionPurityAnalysis / CallGraphAnalysis

| 等级 | 实现范围 |
| --- | --- |
| L1 | 建直接调用图 |
| L2 | 区分 runtime/external call 为 impure |
| L3 | 迭代推导 pure/readonly/impure |
| L4 | 处理递归 SCC；给 DCE/Inline/LICM 统一使用 |

验收：

```text
putint/starttime/stoptime 标记 impure
只做算术返回的函数 pure
调用 impure 函数的函数 impure
递归 SCC 保守处理
```

## 11. AliasAnalysis

| 等级 | 实现范围 |
| --- | --- |
| L1 | 不同 alloca 标量互不别名 |
| L2 | 不同全局变量互不别名；未知指针保守 may-alias |
| L3 | GEP 基址相同但常量 offset 不同时区分 |
| L4 | 循环内数组访问的简单区间/步长别名分析 |

依赖：

```text
指针基址提取
GEP/数组索引规范化
SCEV 可选
```

验收：

```text
a[i] 和 a[i] must-alias
a[i] 和 b[i] no-alias
a[0] 和 a[1] no-alias
unknown call 后内存状态失效
```

## 12. LoadElimination / StoreToLoadForwarding

| 等级 | 实现范围 |
| --- | --- |
| L1 | 同一基本块内连续重复 load 消除 |
| L2 | store 后紧跟 load 转发 |
| L3 | 跨基本块，在支配路径上消除 load |
| L4 | 利用 AliasAnalysis，遇到 may-write call 或 may-alias store 才失效 |

验收：

```text
load p; load p -> 第二个替换
store x,p; load p -> x
跨块 dominated load 替换
遇到未知 call 不替换
```

## 13. DeadStoreElimination

| 等级 | 实现范围 |
| --- | --- |
| L1 | 同块内连续 store 到同一地址，前一个删除 |
| L2 | store 后没有任何 load 且地址局部不可逃逸，删除 |
| L3 | 跨基本块 DSE，使用支配/后支配关系 |
| L4 | 使用 AliasAnalysis 和 MemoryDefUse |

验收：

```text
store 1,p; store 2,p -> 删除 store 1
局部变量最终 store 后从未读取 -> 删除
可能被 call 读取的 store 保留
```

## 14. LoopAnalysis / LoopSimplify / LCSSA

| 等级 | 实现范围 |
| --- | --- |
| L1 | 用 backedge 识别自然循环 |
| L2 | 建 loop header/latch/body/preheader |
| L3 | LoopSimplify：唯一 preheader、规范 latch、规范 exit |
| L4 | LCSSA：循环内定义、循环外使用通过 exit phi |

依赖：

```text
CFG
DominatorTree
phi 工具
```

验收：

```text
while/for 识别为自然循环
嵌套循环层级正确
没有 preheader 时能创建
循环外使用值通过 LCSSA phi
```

## 15. LICM

| 等级 | 实现范围 |
| --- | --- |
| L1 | 外提循环不变的纯算术指令 |
| L2 | 外提比较、类型转换、地址计算 |
| L3 | 利用 AliasAnalysis 外提安全 load；sink 循环后只用值 |
| L4 | aggressive LICM：结合 LoopSimplify/LCSSA，支持嵌套循环和部分 store 外提 |

依赖：

```text
LoopAnalysis
LoopSimplify
DominatorTree
AliasAnalysis
FunctionPurityAnalysis
```

验收：

```text
循环内 x+1 外提
循环不变数组地址计算外提
可能被 store/call 改写的 load 不外提
嵌套循环外提到正确层级
```

## 16. IndVarSimplify / SCEV

| 等级 | 实现范围 |
| --- | --- |
| L1 | 识别基本归纳变量：`i = phi(init, i+step)` |
| L2 | 识别 trip count 为常量的循环 |
| L3 | 替换派生归纳变量 |
| L4 | SCEV AddRec；给 LSR/Unroll/LoopElim 使用 |

验收：

```text
for i=0;i<N;i++ 识别 i
i*4 识别为派生归纳变量
常量 trip count 可提供给 unroll
```

## 17. LoopStrengthReduce

| 等级 | 实现范围 |
| --- | --- |
| L1 | 循环内 `i * const` 改递增加法 |
| L2 | 数组地址 `base + i * stride` 递推 |
| L3 | 多个相同 stride 共享递推 |
| L4 | 成本模型，避免增加寄存器压力 |

验收：

```text
每轮乘法变成加法递推
数组索引地址计算减少乘法
寄存器压力过大时不做
```

## 18. LoopUnroll

| 等级 | 实现范围 |
| --- | --- |
| L1 | 常量小 trip count 完全展开 |
| L2 | 常量 trip count 部分展开，处理 remainder |
| L3 | 运行时 trip count 部分展开，加 prolog/epilog |
| L4 | 成本模型：循环大小、热点、向量化可能性、代码膨胀限制 |

依赖：

```text
LoopSimplify
LCSSA
SCEV/trip count
CFG clone
```

验收：

```text
for i=0..3 完全展开
for i=0..N 按 4 部分展开
remainder 正确处理
展开后 BasicClean 能删除冗余
```

## 19. LoopElimination

| 等级 | 实现范围 |
| --- | --- |
| L1 | 删除不可达循环 |
| L2 | 删除无副作用且结果未使用的循环 |
| L3 | 常量 trip count 且可计算结果时替换循环 |
| L4 | 结合 SCEV 推导最终归纳变量值 |

验收：

```text
空循环删除
只更新死变量的循环删除
有 store/call 的循环保留
```

## 20. LoopUnswitch

| 等级 | 实现范围 |
| --- | --- |
| L1 | 识别循环内与循环无关条件 |
| L2 | 克隆循环，把条件提到循环外 |
| L3 | 控制代码膨胀，只处理小循环 |
| L4 | 与 LICM/Unroll 联动，unswitch 后 clean |

验收：

```text
while (...) { if (flag) A else B }
变成 if(flag) while A else while B
```

## 21. RepeatedAccumulationElimination

| 等级 | 实现范围 |
| --- | --- |
| L1 | 识别简单重复加法循环 |
| L2 | 支持常量步长和常量边界 |
| L3 | 支持简单嵌套循环中的累加 |
| L4 | 结合 SCEV 和 LoopElimination 推导闭式结果 |

验收：

```text
sum += c 重复 N 次 -> sum += c*N
无副作用循环被进一步删除
```

## 22. Global / Module Passes

| Pass | L1 | L2 | L3 | L4 |
| --- | --- | --- | --- | --- |
| `TreeShaking` | 删除未调用非 main 函数 | 处理调用图 | 处理递归 SCC | 保护 runtime/export |
| `GlobalDCE` | 删除未使用全局 | 区分可写/只读 | 处理初始化引用 | 与 alias 联动 |
| `GlobalConstProp` | 全局 const 替换 | readonly global 替换 | 跨函数传播 | 配合函数特化 |
| `FunctionSpecialization` | 常量实参克隆 | 多调用点选择 | 成本模型 | 与 inline 联动 |

验收：

```text
main 和 runtime 不被删
无用函数/全局可删
常量实参函数克隆后能触发 SCCP
```

## 23. CodeGenPrepare / LowerIntrinsics

| 等级 | 实现范围 |
| --- | --- |
| L1 | 降低复杂常量、修正后端不支持的操作 |
| L2 | 地址计算规范化 |
| L3 | 分支/比较形态贴近目标机器 |
| L4 | 根据 RISC-V 指令选择偏好调整 IR |

验收：

```text
后端不再遇到未知 opcode
复杂寻址被拆成后端可处理形态
```

## 24. MIR MachineDCE

| 等级 | 实现范围 |
| --- | --- |
| L1 | 删除无 use 的虚拟寄存器定义 |
| L2 | 保留 store/call/branch/ret 等副作用指令 |
| L3 | RA 后删除无效 move |
| L4 | 结合 liveness 做块级机器 DCE |

验收：

```text
无用 addi 删除
store/call 不误删
RA 后 mv x,x 删除
```

## 25. CopyPropagation

| 等级 | 实现范围 |
| --- | --- |
| L1 | 同块内 copy 传播 |
| L2 | 跨块支配路径传播 |
| L3 | RA 前虚拟寄存器 copy coalesce |
| L4 | RA 后物理寄存器安全传播 |

验收：

```text
mv v2,v1; add v3,v2,v4 -> add v3,v1,v4
跨 call/定义冲突时不传播
```

## 26. MachineConstantFold / MachineCSE

| 等级 | 实现范围 |
| --- | --- |
| L1 | 机器级立即数运算折叠 |
| L2 | 同块机器 CSE |
| L3 | 跨块机器 CSE |
| L4 | 结合目标指令成本决定是否保留 |

验收：

```text
li + addi 常量合并
重复 address calc 删除
不会破坏物理寄存器约束
```

## 27. Peephole

| 等级 | 实现范围 |
| --- | --- |
| L1 | 删除 `mv x,x`、无效 addi 0 |
| L2 | 合并 li/addi/slli 简单序列 |
| L3 | 分支反转、跳转链简化 |
| L4 | 分 RA 前/RA 后规则，按目标机器调参 |

验收：

```text
addi x,x,0 删除
j next; next: 删除跳转
beq + j 反转为 bne 减少跳转
```

## 28. BranchSimplify / BlockLayout

| 等级 | 实现范围 |
| --- | --- |
| L1 | 删除跳转到下一块 |
| L2 | 跳转链压缩 |
| L3 | 根据 fallthrough 重排基本块 |
| L4 | 根据分支频率/循环深度布局热点 |

验收：

```text
j L1; L1: 删除 j
if 分支 fallthrough 更合理
循环体布局连续
```

## 29. Register Allocation Related

| 项目 | L1 | L2 | L3 | L4 |
| --- | --- | --- | --- | --- |
| `RegisterAllocator` | 正确分配虚拟寄存器 | 支持 spill | spill cost 考虑循环深度 | copy coalescing |
| `StackObjectPromotion` | 简单栈对象提升 | 消除冗余 load/store | 与 RA 协作 | 与 alias/escape 信息协作 |
| `PostRAScheduler` | 块内简单重排 | 避免 RAW stall | 目标机器 latency 模型 | 压测性能样例调参 |

## 推荐实现顺序

### 第一批：体系必须先有

```text
PassManager changed
PipelineBuilder
CFGSimplify L2
DCE L3
Mem2Reg L4
ConstantFolding L3
LocalScalarCleanup L2
```

### 第二批：打开中端收益

```text
FunctionInlining L2
CSE L2
SCCP L3
Reassociate L2
AliasAnalysis L2
LoadElimination L2
DSE L2
LICM L2
```

### 第三批：循环性能核心

```text
LoopAnalysis L2
LoopSimplify L3
LCSSA L3
LICM L3
IndVarSimplify L3
LoopStrengthReduce L2
LoopUnroll L2
LoopElimination L2
```

### 第四批：后内联和模块优化

```text
CallGraphAnalysis L3
FunctionPurityAnalysis L3
LateFunctionInlining L3
TreeShaking L3
GlobalConstProp L2
FunctionSpecialization L2
ADCE L3
CodeGenPrepare L3
```

### 第五批：后端优化

```text
MachineDCE L2
CopyPropagation L2
MachineConstantFold L2
Peephole L3
BranchSimplify L3
BlockLayout L3
MachineLICM L2
PostRAScheduler L2
```

## 每个 pass 的完成定义

一个 pass 只有同时满足以下条件，才算达到对应等级：

1. 有独立单测或 targeted SysY 样例。
2. 能接入 pipeline。
3. 能单独关闭。
4. 能报告 changed。
5. pass 后 IR/MIR verifier 通过。
6. functional 正确性不回退。
7. 对应等级的验收样例通过。

