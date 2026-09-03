# coact 精简评审报告

## 总评

当前代码基线可构建、`ctest` 50/50 通过，AO/worker 分层和静态资源约束整体清晰；但这只是正常演示路径通过，不能覆盖资源耗尽和并发边界。

最严重问题是 `kRcSync -> kRcResume` 误挂 `rcGoHome`，正常重配在首帧确认前就把 `kRecfgTxn` 改回 `kRunning`，会话门控失效。

## 发现清单

### Critical

1. **`examples/isp_pipeline/recfg_session.hpp:538-539`**：`kRcSync` 到 `kRcResume` 的正常推进弧使用 `rcGoHome`。当 `rcEnterSync` 自提交 `kRecfgStage` 后，该弧会执行终局动作，提前关闭会话窗口；随后 `kFirstFrame` 仍可进入 Commit。失败场景：重配正在等待首帧时，其他 AO 看到 `kRunning` 并接受本应冻结的命令。修复建议：该弧使用仅推进阶段的 action（或 `nullptr`），仅 Quiesce/Commit 的终局弧调用 `rcGoHome`。

### Major

2. **`src/core/pal_rtthread.cpp:533-540,548-557`**：`cond_wait` 在释放互斥量后才增加 `waiters`，`cond_broadcast` 对快照数量逐个 `rt_sem_release`。失败场景：广播发生在 536 与 537 行之间，新 waiter 看不到令牌而永久阻塞。修复建议：用带 mutex 保护的 waiter 注册/唤醒协议，或在 RT-Thread 上实现真正的条件变量语义。

3. **`include/coact/monitor.hpp:664,815-827`、`include/coact/dispatcher.hpp:205-206`**：监控 `AoCounters::pending` 只在提交路径写入，Dispatcher 递减 AO pending 后没有同步更新 monitor。失败场景：队列已排空，`rt.monitor().ao(id).pending` 仍停留在历史高值，基于该指标的排空/熔断判断误报。修复建议：在每次 Dispatcher decrement 后记录当前值，或让 monitor 直接读取同一计数源。

4. **`examples/isp_pipeline/coro_mode.hpp:114-120`、`include/coact/coro/posix.hpp:384,393-416`**：`coro_sleep_us` 以微秒生成 deadline，却被 `run_once` 当纳秒比较。失败场景：任意 worker `sleep_us(50)` 后 deadline 远早于当前 `now_ns()`，协程立即恢复，失去硬件时延与公平性模型。修复建议：统一使用纳秒，或在 `coro_sleep_us` 中将 `now_us()` 和相对时延转换为纳秒。

5. **`examples/isp_pipeline/coro_pal.hpp:88-117`**：`thread_create` 先登记 PendingSlot 即返回，`thread_join` 若 pump 尚未 materialize 会循环超时并把句柄置无效，但不清理 pending 请求。失败场景：启动后立即停机时，pump 随后仍 materialize 一个无人 join 的协程。修复建议：加入显式取消/已 materialize 状态和可靠完成通知，禁止超时后遗留槽位。

6. **`examples/isp_pipeline/isp_chain.hpp:578-589`**：IRQ 完成事件找到 parked 描述符后，`alloc_typed` 失败直接返回，未清空 parked 槽或递减 `in_flight`。失败场景：事件池暂时耗尽时，后续同帧完成被判 stale，停机 drain 永远等不到该在途计数。修复建议：分配失败走明确的 drop/release 弧并收回 parked 所有权。

7. **`examples/isp_pipeline/video_stream.hpp:714-726,766-777`**：SOUT 完成路径同样在输出事件分配失败时直接返回，保留 parked 描述符和 in-flight 计数。失败场景：池耗尽或 overload 时 PIC/TEMP 通道永久滞留，统计不再闭合。修复建议：失败路径必须清槽、减计数并记录一次可观测丢弃。

8. **`src/core/pal_rtthread.cpp:625-639`**：`rt_sem_init` 成功后，`rt_thread_init` 或 `rt_thread_startup` 失败只清 `in_use`，没有 `rt_sem_detach`，也未清理 `user_ctx`/TCB 状态。失败场景：重复创建/启动失败后复用槽位，旧 semaphore/TCB 状态污染后续线程。修复建议：所有失败分支执行与成功路径对称的 detach/reset，再释放槽位。

9. **当前评审对象与工作树不一致**：任务书指定的 SoftIrq 增量 `e6b5f77` 不在当前 `HEAD=f8bd107` 的祖先链，当前树也没有 `src/core/test_softirq.cpp`；因此 SoftIrq 的 signalfd、RT-Thread SPSC 和 5 个测试无法作为当前实现验收。失败场景：把本次 50/50 通过误解为包含 SoftIrq 覆盖。修复建议：先合并/切换到包含 e6b5f77 的评审提交，再重新构建和评审。

### Minor

10. **`examples/isp_pipeline/main.cpp:586,634,657,686,751,901,990,1007,1141`、`examples/isp_pipeline/coro_mode.hpp:138`、`examples/isp_pipeline/coro_pal.hpp:107`**：仍有裸 `int` 循环/计数变量，违反规约的固定宽度整型要求。失败场景：不同 ABI 下计数宽度或符号转换出现不一致。修复建议：按语义改为 `uint16_t`/`uint32_t`，失败计数使用固定宽度类型。

11. **`examples/isp_pipeline/common.hpp:160,170,279,750`、`examples/isp_pipeline/sensor_irsc.hpp:575`**：头文件中的大 `constexpr` 表未统一使用 `inline constexpr`，每个包含 TU 都产生内部副本。失败场景：多个模块包含后固件体积增加、表地址不一致；虽不必然链接冲突，但不符合本仓库表契约。修复建议：统一改为 `inline constexpr`。

12. **`examples/isp_pipeline/recfg_session.hpp:88-140,472-476`**：`IspCtx`、`onIspCmd`、`kIspTransitions` 只定义未实例化，`rcEnterQuiesce`/`rcEnterResume` 也只有声明；实际启动由 orchestrator 直接合成 ack。失败场景：维护者依据注释修改“真实 IspPipelineAO”却没有运行实例覆盖。修复建议：删除死代码，或补一个明确的编排器测试实例并同步文档。

13. **`docs/cpp_coding_conventions_zh.md:8,24,47`、`docs/design_architecture_zh.md:3,316,422-425`**：文档仍大量指向已拆分前的 `examples/isp_pipeline_demo.cpp`，并描述 `session_advance` 可按需广播；当前代码已拆分为 `examples/isp_pipeline/*.hpp`，且 `session_advance` 不调用 `publish()`。失败场景：评审者按旧落点检索不到实现，误判会话广播已启用。修复建议：把落点改为实际模块，并明确当前采用 atomic guard、事件 composite 未启用。

14. **`examples/isp_pipeline/recfg_session.hpp:612-615,856-873`**：`modes_legal()` 与 `SessionEventComposite::publish()` 均有实现但无调用点；这是已知死代码而非完整协议闭环。失败场景：调用方以为非法 cache 组合会被 setter 拒绝、或以为 session 广播已生效。修复建议：删除未启用接口，或在唯一入口强制调用并增加断言。

## 架构建议

1. 为重配 HSM 增加“每一条自驱弧的终局/非终局”静态表检查，避免阶段推进复用终局 action。
2. 将 monitor pending 与 AO 的 `PendingCounter` 绑定为单一观测接口，禁止提交侧镜像计数。
3. 为所有 parked/in-flight 结构建立统一 `drop_pending()` 失败弧，覆盖池耗尽、通道拒绝和停机。
4. 将协程时间基准、join 完成和 stop drain 收敛到一个可测试的生命周期协议。
5. 评审前固定提交基线；SoftIrq 等增量必须出现在同一分支并有对应 ctest 项。

## 验证记录

- `cd build && make -j12`：通过。
- `cd build && ctest --output-on-failure`：50/50 通过。
- MiniMax worker 因 Token Plan 限额（HTTP 429）未返回结果，以上结论均由主模型逐项回读源码核实。
