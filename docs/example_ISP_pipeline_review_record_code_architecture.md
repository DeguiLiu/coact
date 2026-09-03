# coact 代码与架构评审记录

**关联文档**：架构设计见 `example_ISP_pipeline_design_architecture_zh.md`；一致性处理见 `example_ISP_pipeline_design_consistency_avoidance_zh.md`；功能验收见 `example_ISP_pipeline_functional_test_report_zh.md`；最新 coro 运行日志见 `isp_pipeline_demo_run_log_fresh.txt`。

**阅读指引**：本章合并自两轮评审任务书与评审报告。第 1 章说明评审对象和边界口径；第 2 章追踪全部遗留项的修复落地（含修复 commit）；第 3 章是经证据核实的问题清单（按严重度分级）；第 4 章是下轮评审可直接复用的检查表。

## 1. 被评系统与评审口径

`examples/isp_pipeline/`（14 文件）是 RS500 红外视频链路的架构演示：14 个 AO（主动对象）+ 7 个 worker 实例（6 类，其中 IrscWorker 按帧节拍自主产帧、无输入 job 队列），层次 HSM（g_session 会话门控 + RecfgOrchAo / 重配编排器 8 态），三块黑板（DdrCtx 帧数据域 / HwStateBlackboard 硬件状态域 / PeriphRegCache 寄存器镜像域）。默认构建即 coro 模式（单 pthread 绑核协作调度，与 RT-Thread 单核公平对齐），66 项断言 ALL PASS。

`include/coact/` 是基础框架：AO 事件驱动核心（pool/dispatcher/AO/HSM/monitor）+ PAL 双平台层（SemOps/MutexOps/CondOps/ThreadOps/SoftIrqOps）+ timer.hpp + coro/（Linux 专用）。

**评审口径（两轮一致）**：
- demo 定位是"架构模型与故障注入演示"，模拟消息发生级——不把"无真实寄存器位定义 / 无 SPI 传输 / 线程未全覆盖"列为缺陷
- coact 库定位单一（AO 事件驱动框架），不建议扩平台能力面
- 每条发现必须附 文件：行号 证据与失败场景，宁缺毋滥；不确定标 [待核实]

## 2. 遗留项修复追踪

首轮评审（对象 f8bd107）发现 Critical 1 + Major 7 + Minor 6，二轮复核后陆续修复。当前状态（2026-09-04，基线 HEAD 30bce9b，ctest 52/52）：

| 编号 | 问题 | 状态 | 修复落点 |
|---|---|---|---|
| C-1 | rcGoHome 误挂 kRcSync→kRcResume 弧，事务在首帧确认前关闭会话窗口 | **已修复** | 7de7094：该弧 action 改 `nullptr`，rcGoHome 仅保留 Quiesce reject 与 Commit home 两条终局弧 |
| M-1 | RT-Thread cond_broadcast 快照与释放间存在唤醒丢失窗口 | **已修复** | 7de7094：waiters 计数先增后释放 + cond_signal 按 waiters 判空门控 |
| M-2 | RtThread() 默认构造引入 16KiB 静态资源 | 部分修复 | 注释已声明生产板须传显式资源；默认构造仍实例化（宿主路径可接受，板级按需删除） |
| M-3 | 头文件 constexpr 表缺 inline（kProductModes/kCaliModes/kLat/kSigNames/kIrscDoneTable） | **已修复** | 7de7094 + 9ef5b88：全部 `inline constexpr` |
| M-4 | 裸 int 循环计数（main.cpp 9 处等） | **已修复** | 9ef5b88：`uint32_t` 全量替换 |
| M-5 | RT-Thread 线程初始化失败不清理内核对象 | **已修复** | 7de7094：失败路径补 `rt_sem_detach` 对称清理 |
| M-6 | kRcPrecheck 不可达占位状态 | 已知保留 | 注释标注 UNREACHABLE (historical)；枚举占位不影响派发 |
| M-7 | 死代码（IspPipelineAO 表 / SessionEventComposite::publish / modes_legal） | 已知保留 | 文档标注备用机制；下轮可清理 |
| m-2 | CoroPal thread_join 忙等轮询 | 部分修复 | 200us 步进已从 sleep 改 yield；固定上限仍在 |
| m-3 | coro 模式 worker 走真 cond_wait 破坏单核公平 | **已修复** | coro_pal.hpp pump 路径解锁→yield→trylock |
| m-4 | 文档落点指向旧单文件 | **已修复** | 文档已改多文件结构与 example_ 前缀 |

**评审期间新增修复（非首轮清单）**：
- Monitor pending 不回写：dispatcher 递减路径补 `record_pending`（7de7094）
- 协程 deadline 微秒/纳秒混用（睡眠近似立即到期）：统一纳秒基准（2c28d56）
- **协程 current 路由崩溃（#41 blocker）**：demo 层 current_ 仅在协程 body 入口设置一次，多协程交替后读到陈旧指针，在自己栈上对错误协程 yield → swapcontext 互踩 → canary 爆。修复为 executor 在每次 swap 前后维护 current（4bb4e57，消融实验坐实：仅移植路由修复即 3/3 PASS）
- slot 复用缺陷与 running_ 跨线程竞争：arm 重置状态 + atomic（4bb4e57，`rearms_retired_slot` RED 坐实）
- SoftIrq sigmask 泄漏：deinit 路径补 SIG_UNBLOCK（9ef5b88）

**仍未闭合**：IRQ/SOUT parked 所有权在池耗尽时不闭合（分配失败直接 return，in_flight 不减）——见第 3 章 #6/#7。

## 3. 发现清单（按严重度）

### Critical（历史，均已修复）

1. **HSM 事务门控提前关闭**（已修，见第 2 章 C-1）。教训：事务 HSM 的"阶段推进"和"终局回 Idle"必须是不同的静态 action。

### Major（当前仍存在，按风险排序）

2. **RT-Thread 条件变量仍是近似协议**：M-1 修复消除了快照窗口，但 waiter 注册/检查/令牌交付未在同一互斥协议下完成，`cond_signal` 空闲时可能遗留令牌改变下一次 wait 语义。调用方须用 while-loop 纪律（PAL 注释已声明）。
3. **协程 stop/join 可遗留孤儿**：coro_pal.hpp PendingSlot 在 pump 未 materialize 时无法完成 join；固定 2,000,000 次轮询后仅置句柄失效，pending 仍可被 pump arm。有界 drain（20,000 次 run_once）不检查 live_count()，长睡眠协程可在 stop 返回时仍存活。
4. **IRQ parked 所有权在池耗尽时不闭合**：isp_chain.hpp 找到 parked 项后，完成事件分配失败直接 return，in_flight 与槽位不变，下次同 frame 完成进 stale 分支。
5. **SOUT parked 所有权同样不闭合**：video_stream.hpp PIC/TEMP 两路输出事件分配失败直接 return，停机守卫可能超时。修复方向：统一清槽、减 in-flight、增 drop 计数。
6. **DdrCtx 所有权协议字段非原子**（[待核实]）：单 Dispatcher 路径不触发；若未来并行化读者，check 到 claim 之间可被 writer 覆盖。要么删除"未来多线程仍安全"承诺，要么 CAS 保护 check-and-claim。
7. **Monitor 无 dispatched/processed 累计计数**：msh_monitor_demo 只能用 ctx 计数补位；框架层缺此维度。

### Minor

8. CoroPal thread_join 忙等轮询（200us yield 步进 + 固定上限，见第 2 章 m-2）。
9. 死代码（M-7：IspPipelineAO / publish / modes_legal）下轮清理。
10. `wrape.frames_framed` 主线程读 / Dispatcher 写的业务竞态（取证 agent 发现，独立于协程修复；coro 模式跑通后 TSan 全量扫描会标记）。

### 架构建议（保留 5 条）

1. 事务 HSM 的"阶段推进"与"终局回 Idle"定义成不同静态 action 类型，加表级静态检查。
2. parked descriptor、in-flight 计数、池分配失败统一为一个所有权状态机，禁止裸 return。
3. PAL 条件变量优先可证明的 mutex+predicate 协议，不用 waiter 快照近似 broadcast。
4. 协程 executor、CoroPal、worker stop 共用一套生命周期测试：pending/armed/running/retired 四态可观测。
5. 评审基线（HEAD、增量 commit、ctest 列表）写入报告前置，避免增量提交脱离评审范围。

## 4. 下轮评审 checklist

按顺序执行，每项给 文件：行号 证据：

**A 架构**
- [ ] AO/worker 边界纯度：AO 只做事件驱动状态推进；worker 只产生完成事件（submit_from_task），不做状态决策
- [ ] 三块黑板五要素闭环（写者/读者/同步/失效/所有权）；14 条 AO 事件队列与 6 条 worker 输入队列区分
- [ ] 会话门控与 RecfgOrchAo 8 态关系；终局弧覆盖全部终局；无可达无弧处理的状态
- [ ] PAL install/take/release/deinit 配对对称性；RT-Thread CondOps 复刻语义
- [ ] coro 与 RT-Thread 编译期隔离彻底（coro 不得泄漏进 rtthread 编译单元）

**B 并发正确性**
- [ ] EventPool tagged-CAS ABA 防护；submit_from_task vs submit_from_isr 使用恰当
- [ ] DdrCtx 槽位 claim/release 窗口分析；coro 切点对象生命周期

**C 代码质量**
- [ ] 对照 cpp_coding_conventions_zh.md 47 项（固定宽度整型/常量左侧/Allman/零堆）
- [ ] 头文件卫生：ODR 风险（hpp 非 inline 非模板定义）；过度设计

**D 测试与文档**
- [ ] main.cpp 66 项断言无恒真/弱断言；ctest 覆盖缺口
- [ ] 架构文档与代码漂移抽查 3-5 处

**输出格式**：总评（3 行内）→ 遗留项复核表（第 2 章逐条标已修/未修）→ 发现清单（Critical/Major/Minor，每条含失败场景与一句话修复建议）→ 架构建议 ≤5 条。规模 10-20 条，宁缺毋滥。
