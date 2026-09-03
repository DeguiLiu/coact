# coact 代码与架构评审报告

## 总评

基线构建和现有 50 项 ctest 全部通过，AO/worker 事件边界、静态池和 HSM 表驱动方向基本健康；但资源耗尽、协程时钟和 RT-Thread 条件语义仍有明显闭环缺口。

最严重问题：`kRcSync -> kRcResume` 误调用 `rcGoHome`，重配在首帧确认前关闭 `RECFG_TXN` 门控。

## 首轮遗留项复核

| 项目 | 状态 | 当前证据与结论 |
|---|---|---|
| C-1 rcGoHome 误挂 | 未修复 | `examples/isp_pipeline/recfg_session.hpp:538-539` 仍为 `kRcSync -> kRcResume, ..., rcGoHome`；该弧不是终局。 |
| M-1 cond_broadcast 窗口 | 未修复 | `src/core/pal_rtthread.cpp:533-557` 仍先解锁、后增 waiter，再按快照释放 semaphore。 |
| M-2 RtThread 默认 16KiB | 部分修复 | `src/core/pal_rtthread.cpp:20-27` 已明确生产板应传显式资源，但默认构造仍实例化 `RtThreadResources<16384U,...>`，宿主/误用路径仍承担静态开销。 |
| M-3 头文件 constexpr 表 | 未修复 | `common.hpp:160,170,279,750` 与 `sensor_irsc.hpp:575` 仍非 `inline constexpr`。 |
| M-4 裸 int | 未修复 | `main.cpp:586,634,657,686,751,901,990,1007,1141`、`coro_mode.hpp:138`、`coro_pal.hpp:107` 仍存在。 |
| M-5 RT-Thread 线程槽清理 | 未修复 | `src/core/pal_rtthread.cpp:625-639` 初始化/启动失败只回退 `in_use`，没有 semaphore detach/reset。 |
| M-6 kRcPrecheck 不可达 | 未修复 | `recfg_session.hpp:457-464,486-492` 保留占位状态，无任何转移进入 `kRcPrecheck`。 |
| M-7 死代码 | 未修复 | `recfg_session.hpp:88-140` 的 IspPipeline 表、`common.hpp:856-873` 的 publish、`recfg_session.hpp:612-615` 的 modes_legal 均无调用。 |
| m-1 cond_signal 遗留令牌 | 未修复 | `src/core/pal_rtthread.cpp:543-546` 无 waiter 判断，空闲 signal 会在 counting semaphore 中留下令牌，改变下一次 wait 语义。 |
| m-2 CoroPal 忙等 | 部分修复 | `coro_pal.hpp:124-140` 的 cond_wait 已 cooperative yield；但 `:104-117` 的 thread_join 仍以 200us 睡眠轮询并以固定次数超时。 |
| m-3 coro 真 cond_wait 破坏公平 | 已修复 | `coro_pal.hpp:131-142` 在 pump 协程路径解锁、yield、trylock；非 pump 线程才调用 Posix cond_wait。 |
| m-4 文档旧单文件落点 | 已修复 | 架构文档已改用 `examples/isp_pipeline/` 和 `example_ISP_pipeline_*` 文档名。 |
| m-5/m-6 | [待核实] | 首轮任务书未给出这两项的唯一原始定义；当前未将其臆测映射到新问题，需补充首轮报告后复核。 |

## 发现清单

### Critical

1. **HSM 事务门控提前关闭**：`examples/isp_pipeline/recfg_session.hpp:538-539` 的 Sync→Resume 弧调用 `rcGoHome`。`rcEnterSync` 把镜像置为 Resuming 后自提交阶段事件，该 action 会执行 `session_advance(kRunning)`；在 `kFirstFrame` 到达前，子 AO 已观察到非事务态。修复：将该 action 改为阶段推进 action，终局仅保留 Quiesce reject 与 Commit home。

### Major

2. **RT-Thread 条件广播不是原子快照**：`src/core/pal_rtthread.cpp:533-557` 在 waiter 注册与 semaphore take 之间存在广播窗口；`cond_signal` 也不区分是否存在 waiter。触发时序是 signal/broadcast 与新 waiter 交错。修复：让 waiter 注册、状态检查和 token 交付在同一互斥协议下完成。

3. **Monitor pending 永不回写**：`include/coact/coordinator.hpp:211-235` 只在 enqueue 后 `record_pending`，而 `include/coact/dispatcher.hpp:205-206,230-233` 只 decrement AO counter。停止后 AO pending 为零但 Monitor pending 仍为旧值。修复：统一从 `PendingCounter` 读取，或在每次递减后同步记录。

4. **协程 deadline 微秒/纳秒混用**：`examples/isp_pipeline/coro_mode.hpp:96-119` 的 `now_us()+us` 与 `include/coact/coro/posix.hpp:384,393-416` 的 `now_ns()` 比较。任意 sleep 请求都会近似立即到期，造成忙跑和不真实的完成顺序。修复：deadline 全部使用 `uint64_t` 纳秒。

5. **协程 stop/join 可遗留孤儿**：`examples/isp_pipeline/coro_pal.hpp:88-117` 的 PendingSlot 在 pump 尚未 materialize 时无法完成 join；固定 2,000,000 次轮询后仅让句柄失效，pending 仍可被 pump arm。修复：为 pending 请求增加取消标记、完成信号和 stop 统一回收。

6. **IRQ parked 所有权在池耗尽时不闭合**：`examples/isp_pipeline/isp_chain.hpp:564-589` 找到 parked 项后，如果完成事件分配失败直接返回，`in_flight` 和槽位不变。下一次同 frame 完成会进入 stale 分支。修复：用 drop 弧释放 parked 项并计数。

7. **SOUT parked 所有权同样不闭合**：`examples/isp_pipeline/video_stream.hpp:704-726,755-777` 输出事件分配失败直接 return。PIC/TEMP 两条路径都会卡住 in-flight，停机守卫可能超时。修复：统一清槽、减 in-flight、增加 reject/drop 计数。

8. **RT-Thread worker 创建失败泄漏内核对象状态**：`src/core/pal_rtthread.cpp:625-639` 在 semaphore 已初始化后，后续失败未 detach；`user_ctx` 仍指向调用方句柄。重复创建时会把未清理 TCB/semaphore 当成新槽。修复：失败路径与 join 路径对称清理全部资源。

9. **DdrCtx 所有权协议仅是普通字段**：[待核实，当前单 Dispatcher 路径不触发] `examples/isp_pipeline/common.hpp:617-638` 在校验后才写 `owner=kReaderClaimed`，`owner`/`slot_frame`/stamp 都不是原子。若未来真的按注释并行化读者，校验到 claim 之间可被 writer 覆盖并产生数据竞争。修复：要么删除“未来多线程仍安全”的承诺，要么以 CAS/锁保护 check-and-claim。

10. **评审对象缺少 SoftIrq 增量**：`e6b5f77` 不在当前 `HEAD=f8bd107` 祖先链，`src/core/test_softirq.cpp` 和对应 PAL 实现当前不存在；现有 50 项测试不覆盖任务书所说的 SoftIrq。修复：锁定包含 e6b5f77 的提交后重新执行本报告的 A4/C2/D2。

### Minor

11. **规约中的裸整型残留**：`main.cpp` 多处 drain loop 和 `fails`、`coro_mode.hpp:138`、`coro_pal.hpp:107` 使用 `int`；与 `cpp_coding_conventions_zh.md` 的固定宽度整型规则不符。修复：改用有明确范围的 `uint16_t/uint32_t`。

12. **头文件常量表未 inline**：`common.hpp:160,170,279,750`、`sensor_irsc.hpp:575` 为非 inline constexpr 定义。C++ 语义上不一定造成链接冲突，但会产生 TU 副本并违背表契约。修复：统一 `inline constexpr`。

13. **IspPipelineAO 代码与实际装配脱节**：`recfg_session.hpp:88-140` 定义了 8 节点级联和 HSM 表，但 main 实际用 orchestrator 合成 8 个 ack；文档虽注明“未实例化”，仍保留可误用 API。修复：删除死代码或把合成行为封装成唯一明确的测试 double。

14. **文档机制漂移**：`docs/example_ISP_pipeline_design_architecture_zh.md` 已改为当前模块结构，并明确 session guard 直读 atomic、事件广播为备用机制。

15. **有界协程 drain 可能提前返回**：`examples/isp_pipeline/coro_mode.hpp:137-142` 固定最多 20,000 次 `run_once()`，不检查最终 `live_count()`。长 sleep 或尚未 materialize 的协程可在 stop 返回时仍存活。修复：stop 只在 `live_count()==0` 后返回，并提供取消/时间推进策略。

## 架构建议

1. 把事务 HSM 的“阶段推进”和“终局回 Idle”定义成不同的静态 action 类型，并加入表级静态检查。
2. 将 parked descriptor、in-flight 计数和池分配失败统一为一个所有权状态机，禁止裸 `return`。
3. PAL 条件变量应优先采用可证明的 mutex+predicate 协议，再映射 RT-Thread semaphore；不要用 waiter 快照近似 broadcast。
4. 协程 executor、CoroPal、worker stop 共用一套生命周期测试：pending、armed、running、retired 四态必须可观测。
5. 将评审基线（HEAD、增量 commit、ctest 列表）写入报告前置记录，避免 SoftIrq 等提交脱离评审范围。

## 验证、覆盖与精简结论

- 基线命令：`cd build && make -j12 && ctest --output-on-failure`。
- 结果：构建成功，50/50 测试通过。
- 现有 ctest 已覆盖 coro registry/combinators/posix/awaitable/scheduler/integration、timer 和 RT-Thread stub；当前工作树未覆盖 SoftIrq（目标增量缺失）。
- MiniMax worker 因 Token Plan 限额返回 HTTP 429；所有报告证据由主模型使用 `rg`/带行号读取复核。

### 精简结论

当前代码可构建且测试通过，但资源耗尽、PAL 条件语义、协程时钟、IRQ 完成失败和停机排空仍需持续回归。
