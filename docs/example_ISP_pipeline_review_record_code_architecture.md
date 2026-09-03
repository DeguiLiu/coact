# coact 代码与架构评审记录

**关联文档**：架构设计见 `example_ISP_pipeline_design_architecture_zh.md`；一致性处理见 `example_ISP_pipeline_design_consistency_avoidance_zh.md`；功能验收见 `example_ISP_pipeline_functional_test_report_zh.md`；最新 coro 运行日志见 `isp_pipeline_demo_run_log_fresh.txt`。

**阅读指引**：本章合并自两轮评审任务书与评审报告。第 1 章说明评审对象和边界口径；第 2 章追踪全部遗留项的修复落地（含修复 commit）；第 3 章是经证据核实的问题清单（按严重度分级）；第 4 章是下轮评审可直接复用的检查表；第 5 章为历史评审裁决附录。

## 1. 被评系统与评审口径

`examples/isp_pipeline/`（14 文件）是 RS500 红外视频链路的架构演示：14 个 AO（主动对象）+ 7 个 worker 实例（6 类，其中 IrscWorker 按帧节拍自主产帧、无输入 job 队列），层次 HSM（g_session 会话门控 + RecfgOrchAo / 重配编排器 8 态），三块黑板（DdrCtx 帧数据域 / HwStateBlackboard 硬件状态域 / PeriphRegCache 寄存器镜像域）。默认构建即 coro 模式（单 pthread 绑核协作调度，与 RT-Thread 单核公平对齐），66 项断言 ALL PASS。

`include/coact/` 是基础框架：AO 事件驱动核心（pool/dispatcher/AO/HSM/monitor）+ PAL 双平台层（SemOps/MutexOps/CondOps/ThreadOps/SoftIrqOps）+ timer.hpp + coro/（Linux 专用）。

**评审口径（两轮一致）**：
- demo 定位是"架构模型与故障注入演示"，模拟消息发生级——不把"无真实寄存器位定义 / 无 SPI 传输 / 线程未全覆盖"列为缺陷
- coact 库定位单一（AO 事件驱动框架），不建议扩平台能力面
- 每条发现必须附 文件：行号 证据与失败场景，宁缺毋滥；不确定标 [待核实]

## 2. 遗留项修复追踪

首轮评审（对象 f8bd107）发现 Critical 1 + Major 7 + Minor 6，二轮复核后陆续修复。当前状态（基线 HEAD 30bce9b，ctest 52/52）：

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
- **协程 current 路由崩溃（#41 blocker）**：demo 层 current_ 仅在协程 body 入口设置一次，多协程交替后读到陈旧指针，在自己栈上对错误协程 yield → swapcontext 交叉执行 → 栈保护字节被覆盖。修复为 executor 在每次 swap 前后维护 current（4bb4e57，消融实验验证：仅移植路由修复即 3/3 PASS）
- slot 复用缺陷与 running_ 跨线程竞争：arm 重置状态 + atomic（4bb4e57，`rearms_retired_slot` 失败测试验证）
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

## 5. 附录：一致性文档评审回应与裁决表（终裁）

对《design_consistency_avoidance_review_zh.md》五项结论的逐条裁决（按重构完成后的最终源码逐条复核，标注修复状态）。评审文档作为历史输入保留不改；裁决为"部分成立"的条目已在一致性文档正文相应位置修正。

| # | 评审条目 | 裁决 | 证据 | 处理 |
|---|---|---|---|---|
| 1 | 重配 HSM 文字/图/转移表不一致；事务窗口提前关闭 | **部分成立**（文档侧已修；代码侧已修复） | `kRecfgTransitions` 现为 11 弧（7 业务 + 4 stale 吸收）；终局弧新增 `at_reject_home`/`at_commit_home` 姿态 guard，陈旧自驱事件不再误杀活事务（已于重构修复）。`rcGoHome` 曾复用于三条终局且 `Syncing→Resuming` 弧误挂该 action，已修复：现只挂两条终局弧；事务窗口已由终局动作自驱关闭（见一致性文档 3.2）。`kRcPrecheck` 仍不可达、`RecfgStage::kCommitted/kFailed` 仍无 HSM 状态（如实描述，见一致性文档 3.2 结构说明第 2 条） | 3.2 节重写为与源码逐弧一致并区分"已修复/未修复" |
| 2 | Dispatcher 串行化与 `std::exchange` 保证过强，混淆 RTC 原子性/事务隔离/并发原子 | **仍成立**（原文表述如此） | 原文"任何其他执行流都无法在事件处理的间隙观察到半改状态"未限定单 RTC 步骤；`std::exchange` 非原子（C++ 标准无并发保证） | 一致性文档第 2 章重写为三层概念区分 + 保证边界声明；编程纪律文档纪律清单相应措辞修正（文档侧修复） |
| 3 | 恒真断言、日志语义相反、修复侧未断言 | 初裁成立，后续已修复 | Scenario A/B/C 与 U8 均增加故障侧和处理侧断言 | 以当前源码和测试输出为准 |
| 4 | "16 个 AO 与 4 个非 AO worker"数量不符 | **仍成立**（原文如此；评审引用的"14 AO/7 worker"与当前源码一致） | `rt.bind()` 共 14 个 AO；worker 实例 7 个（6 类）：IrscWorker（无输入 job 队列）、UsbDmaWorker（单槽），CmdDmaWorker/IspIrqWorker×2/SoutDmaWorker/MipiIrqWorker（WorkerBase 环深 4/2/3/2）；PIC/TEMP FSM 与打包器已合并 | 一致性文档 1.1.1 改为按角色分工重写，完整映射表移至架构文档 §3.5 |
| 5 | "九类都是两份拷贝"覆盖过窄；U1/U3 证据混用 | **仍成立** | U7（能力差异）、U8（容量预算）不存在两份记录；U1 证据域为 5537280/22149120B（运行态重配），U3 为 655360/2621440B（T37 链路），仅数学形态同为 25% | 一致性文档第 1 章升级为"分布式状态契约不一致"三分法；两份拷贝降为具象案例；3.2/3.3 证据域表述分离（文档侧修复） |

另两条次要评审建议的处理：WRAPE 写者归属（一致性文档 3.3 已改为"demo 驱动器写、WRAPE 消费"并指出与 U4 旁路同构）；regcache 表述（`PeriphRegCache` 已限定为"regmap 风格最小协议"）。Guard 一等公民化、状态机函数分层重命名、三块显式黑板属于代码重构方向：三块黑板已在架构文档 §2.2 落地（文档侧），终局 action 按姿态拆分与 `AddrCache::query` guard/失效拆分经最终源码核实尚未落地；重构中新增的终局弧姿态 guard与 stale 吸收弧已部分兑现"成功与拒绝路径都显式进入转移表"的要求（拒绝路径的入口仍是 action 内分支，但终局走声明弧且陈旧事件被吸收）。
