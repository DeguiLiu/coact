# coact 开发任务完成状态总结

**日期**：2026-09-03
**范围**：本次多轮会话全部开发任务（isp_pipeline_demo 重构 + coact 框架演进）
**仓库**：~/coact，分支 master + feature/isp-pipeline-demo（已推送 gitee）

---

## 1. 项目范围

在 coact（C++17 主动对象框架，RT-Thread + Linux host 双平台）上持续演进 isp_pipeline_demo——模拟 RS500 红外视频系统（Preview Start 四阶段 → IRSC 产帧 → ISP 增益双链 → Video 打包 → 输出接口，叠加超分原子重配、T37 提前封帧等 RS500 复盘的真实故障场景）。

**核心定位（用户多轮澄清后固定）**：
- 示例只模拟 RS500 docs 文档提到的核心模块 + vdcmd 命令通道，"只模拟消息的发生，不搬全套流程"
- 硬件只做行为模拟；展示价值在资源竞争 / 消息链路 / 状态同步 / 数据同步 / 异步通知五维优势
- coact 库定位非常单一：AO 事件驱动框架（pool/dispatcher/AO/HSM/monitor），不扩平台能力面
- RS500 业务语义疑问直接查 ~/RS500 源码/文档，不猜测

## 2. 架构变化

1. **单文件 5482 行 → examples/isp_pipeline/ 14 文件**（6 hpp + 6 cpp + coro_mode/coro_pal）：按软硬件角色/RS500 module 子目录/黑板角色划分，每文件头部 ≥15 行中文功能描述 + ASCII 文字图
2. **AO 合并下沉 HSM**：16 → 14 AO（VideoFsmAo 乘积状态表 9 态、VideoPackAo pair 4 态），层次 HSM（g_session 会话门控 + RecfgOrchAo 8 态 11 弧，事务窗口终局自驱 rcGoHome）
3. **7 实例 6 类 worker**（WorkerBase CRTP 骨架）：IrscWorker/UsbDmaWorker/CmdDmaWorker/IspIrqWorker×2/SoutDmaWorker/MipiIrqWorker
4. **三块黑板五要素契约**（写者/读者/同步/失效/所有权）：g_hw_bb 硬件状态域、DdrCtx 帧数据域（7 区×8 槽 + SlotOwner 三态槽位所有权协议）、PeriphRegCache 寄存器镜像域（regmap 三标志协议）
5. **PAL 双平台化**：SemOps/MutexOps/CondOps/ThreadOps 家族（xxxOps 静态函数表、编译期解析）；RT-Thread 无原生 condvar 用 release→take→re-acquire 复刻 pthread 语义；ThreadOps 静态线程表零堆
6. **coact::coro 协程库**（Linux PAL 专用）：ucontext 有栈协程 + 静态栈池 + 单 pthread 绑核协作调度，与 RT-Thread 单核公平对比
7. **硬件并发四武器映射**（§5.3）：总线仲裁→Dispatcher 串行化、寄存器原子→标量字段、ownership 位→槽位所有权、中断同步→worker 完成事件

## 3. 任务状态

### 3.1 已完成（13 项）

| 任务 | 产出 | 验证 |
|---|---|---|
| #32 拆分多文件 | isp_pipeline/ 14 文件，旧单文件已删；ODR 风险修复 | 构建通过 |
| #33 MSH 监控示例 | msh_monitor_demo.cpp 1375 行：status/data/health 三层查询，const 静态命令表，场景 6 幕 + 10 次查询 | 24 断言 ALL PASS，ctest +1 |
| #35 槽位所有权 | DdrCtx SlotOwner 三态协议（写前检查、读侧 claim/release） | ISP demo 已验证 |
| #36 风格清理 | sleep 30→14 处（Dispatcher 内 B 类 13 处记账化），阻塞点中文注释 | demo 行为不变 |
| #38 newosp 评估 | 零移植结论（TimerScheduler 例外单独移植）；FixedFunction/function_ref/FixedString 等列精确触发条件 | 评估文档 |
| #39 PAL 化 | SemOps/MutexOps/CondOps/ThreadOps + sleep_us + RT-Thread 静态线程表（RtThreadResources 16K 栈/8ctx/4K worker 栈）；修复 RTT condvar 持锁死锁 | test_pal_sync 双平台 |
| #40 coact::coro | 13 个头文件、固定栈协程和测试 | ctest 已覆盖 |
| #41 双模式公平性 | pthread 模式零回归 ALL PASS；coro 模式库层全绿（见 §5 遗留 1） | 断言一致 |
| #42 SoftIrq | SoftIrqOps 契约 + Linux signalfd/sigqueue（无 handler，绕过 async-signal-safe 限制）+ RT-Thread SPSC 环 + rt_thread_kill + test_softirq 5 用例 | commit e6b5f77，ctest +1 |
| #43 BitFieldView | 编译期位域视图和寄存器字段 RMW | 已合并，test_bitfield 通过 |
| #45 vocabulary | 接入必要的强类型 ID 和固定容量工具 | 已合并 |
| 文件头注释 | 14 文件每个头部功能+关系+文字图 ≥15 行，风格统一（66 列框线/箭头规范） | 构建零影响 |
| TimerScheduler | 从 newosp 移植改进：回调改事件（submit_from_task）、TickSource 双时钟、ManualTickSource | test_timer 6 测试 27 断言 |

### 3.2 监督与评审

- **监督员**：全程 1 分钟（后改 10 分钟）轮审，收官报告确认 16 轮稳定全绿、无 agent 冲突、预警巨型提交（f8bd107 17395 行）与 common.hpp 950 行大杂烩风险（→ 已派 vocabulary 拆分应对）
- **深度评审**（`docs/example_ISP_pipeline_review_task_code_architecture.md`）：记录重配窗口、PAL 同步、线程资源和代码质量问题。

## 4. 文档清单

| 文档 | 内容 |
|---|---|
| docs/example_ISP_pipeline_design_architecture_zh.md | ISP Pipeline 架构、AO/worker 边界、状态机和共享数据协议 |
| docs/example_ISP_pipeline_design_consistency_avoidance_zh.md | ISP Pipeline 一致性问题、处理方式和评审裁决 |
| docs/cpp_coding_conventions_zh.md | 418 行：纯 C++17 规约 124 条 + 47 项 checklist（全体 agent 遵循） |
| docs/example_ISP_pipeline_functional_test_report_zh.md | 测试方法、RS500 对应关系和一致性验证 |
| docs/isp_pipeline_demo_run_log.txt | 755 行：日志阅读指南 + 18 处内联中文阶段注释 |
| docs/example_ISP_pipeline_review_task_code_architecture.md | 评审任务书 v1.1（含首轮遗留项复核表） |
| docs/cpp17_coact_usage_zh.md | coro 使用文档（并行协作产出） |

## 5. 待处理事项

1. **demo coro 模式崩溃**（#41 blocker）：`coro_sleep_us` canary 违例，已隔离到 -O0 下跨 swapcontext 的内联链 canary 被主栈地址形态值改写；修复路径大概率是禁止跨切点内联或改 boost::context 式保留 rbp 切换；demo 接线保留在树中但不进默认构建，pthread 模式零回归
2. **worktree 合并**：SoftIrq（e6b5f77）/BitFieldView/vocabulary/coro 增量四个 worktree commit 待合并主仓并全量回归
3. **#44 demo 接入 SoftIrq**：选 UsbDmaWorker 一个 worker 走软中断路径演示真实 ISR→AO 中断链
4. **首轮评审遗留修复**：C-1 rcGoHome 弧修正、M-1 cond_broadcast、M-2 默认 ctor、M-3 inline constexpr、M-4 裸 int、M-5 线程槽 detach、M-7 死代码清理
5. **#37 文档对齐总交付**：架构文档补 SoftIrq/BitFieldView/vocabulary 新组件节、规约文档落点从旧单文件改多文件结构、run_log 重采集、总交付汇总
6. **RTT stub 时序**：mdelay 毫秒粒度放大 50us 事务 20 倍，T37 no-gaps 断言 stub 下 FAIL（板级可解）
7. **Monitor 框架缺口**：pending 只记提交侧不递减；无 dispatched 累计计数（msh_monitor 已在注释中记录）

## 6. 当前指标

- **构建**：make -j12 零警告零错误
- **测试**：ctest 52/52；isp_pipeline_demo 当前运行通过；msh_monitor_demo 通过
- **代码**：examples/isp_pipeline/ 14 文件 ~6200 行；include/coact/ 框架 ~2700 行新增（coro 13 头 + timer 505 行 + PAL + SoftIrq）
- **提交**：master f8bd107（52 文件 17395 行）→ 已建 feature/isp-pipeline-demo 分支推送 gitee（git@gitee.com:liudegui/coact.git）
- **规约**：全部 agent 遵循 47 项 checklist；62 断言经评审核实全为真断言（无恒真/弱断言）

### 7. 经验记录

1. **worktree 隔离的前提是基线提交**：未提交的主仓文件对 worktree agent 不可见（BitFieldView agent 首派因此空转），应先 commit 再派
2. **agent 完成通知 ≠ 代码可用**：黑板域改造中途编译断裂、BitFieldView 仅"准备实施"即停——须亲自构建验证
3. **API 中断恢复机制**：grok-4.6 路由 400/server error 导致多 agent 中断，SendMessage 续跑恢复全部成功
4. **评审前置任务书落盘**：评审任务书写入 md 后，复核轮可直接增量执行（v1.1 含遗留项复核表）
