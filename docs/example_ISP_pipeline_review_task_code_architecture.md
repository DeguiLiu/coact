# coact 代码与架构评审任务书

**版本**：v1.1（2026-09-03）
**评审对象**：提交 `f8bd107` 及其后的 worktree 增量改动（SoftIrq e6b5f77、BitFieldView、vocabulary、coro 修复）
**评审模式**：只读评审，不修改任何文件；产出评审报告
**前置动作**：评审 agent 先构建验证基线（`cd build && make -j12 && ctest`，当前应全过），确认评审对象可复现

---

## 1. 被评系统概述

`examples/isp_pipeline/`（14 文件）是 RS500 红外视频链路的架构演示：14 个 AO（主动对象）+ 7 个 worker 实例，层次 HSM（会话门控 + RecfgOrchAo 8 态），三块黑板（DdrCtx 帧数据域 / HwStateBlackboard / PeriphRegCache 寄存器镜像域），槽位所有权协议，重配事务窗口。

`include/coact/` 是基础框架：AO 事件驱动核心（pool/dispatcher/AO/HSM/monitor）+ PAL 双平台层（pal.hpp / pal_posix.hpp / pal_rtthread.hpp，SemOps/MutexOps/CondOps/ThreadOps 家族）+ timer.hpp（TimerScheduler）+ coro/（ucontext 有栈协程，Linux 专用，与 RT-Thread 编译期隔离）。

`src/core/test_*.cpp` 单测 + `docs/` 下架构/一致性/规约/功能测试报告文档。

**背景须知（评审时应尊重的既定边界）**：
- demo 定位是“架构模型与故障注入演示”，模拟消息发生级，不追求硬件保真——不要把“没有真实寄存器位定义 / 无 SPI 物理传输 / 线程未全覆盖”当缺陷报
- coact 库定位单一（AO 事件驱动框架），不要建议扩平台能力面

---

## 2. 评审维度（按顺序执行，每项都要给具体 文件:行号 证据，宁缺毋滥）

### 2.1 架构评审

| # | 评审点 | 说明 |
|---|---|---|
| A1 | AO/worker 边界纯度 | AO 是否只做事件驱动状态推进；worker 是否只做“硬件侧消息发生与回调”。找出业务逻辑泄漏进 worker、或 worker 里做状态决策的地方 |
| A2 | 黑板五要素契约闭环 | 每块黑板（DdrCtx 槽位所有权三态 / HwStateBlackboard / PeriphRegCache 三标志协议）的写者/读者/同步/失效/所有权是否都闭环；找竞态窗口 |
| A3 | HSM 层次设计 | 会话门控与 RecfgOrchAo 8 态关系；事务窗口终局自驱（rcGoHome）是否覆盖全部终局；有无可达但无弧处理的状态 |
| A4 | PAL 抽象质量 | install/take/release/deinit 配对对称性；RtThread CondOps 复刻 pthread 语义的唤醒丢失/超时路径；ThreadOps 静态线程表槽位泄漏 |
| A5 | coro 设计 | ucontext 栈池边界检查；scheduler 单线程 pump 退出路径；coro 与 RT-Thread 编译期隔离是否彻底（不得泄漏进 rtthread 编译单元） |

### 2.2 代码质量评审（先读 docs/cpp_coding_conventions_zh.md，再逐条对照 47 项 checklist）

- B1：(1) 抽查 isp_pipeline 5 个文件 + pal_posix/pal_rtthread + coro/scheduler.hpp，对照 checklist 关键项（固定宽度整型 / 常量左侧 / Allman / 零堆 / rt_kprintf 格式约束等）
- B2：(2) 找违反点：goto / 递归 / RT_ASSERT / 裸 int / 位域滥用 / strcpy 等
- B3：(3) 找过度设计：不必要的抽象层、为假想需求预留的扩展点
- B4：(4) 头文件卫生：include 依赖方向、ODR 风险（hpp 中非 inline 非模板定义）

### 2.3 并发正确性重点审查

| # | 审查点 |
|---|---|
| C1 | EventPool（pool.hpp）tagged-CAS 的 ABA 防护 |
| C2 | Dispatcher 与 worker 线程交界：submit_from_task vs submit_from_isr 使用是否恰当 |
| C3 | DdrCtx 槽位读侧 claim/release 夹 memcpy 的窗口分析 |
| C4 | coro stackful 切点上的对象生命周期（yield 后跨切点的局部引用/指针是否可能悬垂） |

### 2.4 测试与文档一致性

| # | 审查点 |
|---|---|
| D1 | main.cpp 的 62 项断言是否真断言（找出恒真/弱断言） |
| D2 | ctest 覆盖缺口：关键路径（coro scheduler / timer poll / SoftIrq）缺单测 |
| D3 | `example_ISP_pipeline_design_architecture_zh.md` 与代码的漂移点 |

---

## 3. 已完成任务的已知线索（供对照，勿当成定论）

- **coro**：实施 agent 正在排查 `coro_sleep_us` 栈破坏（pump 线程 canary 违例，疑 ucontext 栈基址/限制方向处理）；#41 待完成双模式（pthread/coro）公平性验证、TSan 零 race
- **SoftIrq（已完成，commit e6b5f77）**：pal.hpp SoftIrqOps 契约 + pal_posix signalfd/sigqueue（无 handler）+ pal_rtthread SPSC 环 + rt_thread_kill + test_softirq 5 用例；重点复核：Linux 固定信号 34 的 constexpr 取舍、RT-Thread take 的 1ms 步进轮询语义
- **BitFieldView（已完成）**：include/coact/bitfield.hpp 编译期位域视图 + PeriphRegCache 位域层升级（REG_SOUT_CTRL opcode[4:7]/mode[8:9]/enable[0] 等）+ test_bitfield；复核 RMW 隔离与三标志协议兼容
- **vocabulary（进行中）**：include/coact/vocabulary.hpp 装入 NewType/ScopeGuard/optional/FixedFunction/function_ref/FixedString/FixedVector（newosp 移植，not_null 已裁）；coro/task_id.hpp 与 timer.hpp 手写 ID struct 改 NewType 别名
- **msh_monitor_demo（已完成）**：自查发现 2 项框架缺口——Monitor pending 只记提交侧不递减（AoCounters::pending 冻结在最后提交值不归零，dispatcher 递减路径未回写）；Monitor 无 dispatched/processed 累计计数
- **首轮评审已完成**（评审对象 f8bd107），遗留待修复项，本轮复核是否已修 + 有无新问题：
  - **C-1（Critical）**：rcGoHome 误挂 kRcSync→kRcResume 弧，事务未真正结束就关会话窗口（recfg_session.hpp:339-348/536-541）
  - **M-1**：pal_rtthread cond_broadcast 快照与 rt_sem_release×n 之间有唤醒丢失窗口（src/core/pal_rtthread.cpp:548-558）
  - **M-2**：RtThread() 默认 ctor 引用 16KiB 静态资源，注释与实现矛盾（src/core/pal_rtthread.cpp:24-28）
  - **M-3**：constexpr 头文件表缺 inline（common.hpp kProductModes/kCaliModes/kLat/kSigNames、recfg_session.hpp kIrscCmdSequence/kIrscDoneTable）——注意 vocabulary/BitFieldView agent 可能已顺手修
  - **M-4**：裸 int 循环计数（main.cpp 9 处、coro_pal.hpp:107 等）
  - **M-5**：RT-Thread 线程槽清理缺口（rt_thread_init 成功但 startup 失败 / join 不 detach TCB）
  - **M-6/M-7**：kRcPrecheck 不可达占位、死代码（RecfgCtx/IspPipelineAO/modes_legal/publish）
  - **m-1~m-6**：cond_signal 遗留令牌、CoroPal 忙等、coro 模式下 worker 走真 cond_wait 破坏单核公平、文档落点指向旧单文件等

评审时以上任务 agent 仍在跑，可能正在改上述文件——评审报告应对照**当前工作树实际状态**，不要假设这些改动已合并。

---

## 4. 输出格式（结论先行，规模受控）

1. **总评**（3 行内）：架构健康度一句话 + 最严重的 1 个问题
2. **首轮评审遗留项复核表**：§3 列出的 C-1/M-1~M-7/m-1~m-6 逐条标注「已修复 / 未修复 / 部分修复」（附当前文件:行号证据）
3. **发现清单**：按严重度排序（Critical / Major / Minor），每条含：文件:行号、问题描述、失败场景（什么输入/时序会触发）、修复建议（一句话）
4. **架构建议**：不超过 5 条，每条含理由
5. 规模控制：发现清单 10-20 条，宁缺毋滥；每条必须核实过，不罗列猜测；不确定的标注 [待核实]

## 5. 约束与精简评审要求

- 只读：不得修改任何源文件；构建验证可做，不运行破坏性操作
- 报告用中文，技术风格，结论先行

### 精简版要求

- 评审对象为 `include/coact/`、`examples/isp_pipeline/` 和 `src/core/test_*`。
- 重点检查 AO/worker 边界、共享数据、HSM、PAL、EventPool、协程生命周期、代码规约和文档一致性。
- 示例只模拟消息发生；不把缺少真实寄存器、SPI 传输或完整线程覆盖列为缺陷。
- 报告包含总评、遗留项、10--20 条有证据的发现和不超过 5 条建议。
