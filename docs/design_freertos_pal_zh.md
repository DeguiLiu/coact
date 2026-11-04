# FreeRTOS PAL 移植设计（稳定版基线）

> 日期：2026-11-02
> 状态：阶段 P1（核心 PAL）与 P2（host 契约测试）已实现；P3 板级 bring-up 与 P4 双版本 CI 未做
> coact 基线：`73fe452`（0.6.0）
> FreeRTOS 基线：LTS 202411.00 / Kernel V11.1.0（202406-LTS）
> 兼容验证：Kernel V11.3.3（2026-10-30，最新稳定补丁线）
> 范围：新增 `coact::pal::FreeRtos`；不修改事件模型、AO、HSM、staging、pool 的语义

## 结论先行

coact 的核心已经按编译期 `PalT` 模板化，RT-Thread 只通过 PAL 接触内核，因此 FreeRTOS 移植不需要改核心架构，新增一个 PAL 即可。

工作量分三档：

| 范围 | 内容 | 新增/改动 | 估算 |
|---|---|---|---|
| A. 最小可用 | Runtime + AO/HSM + staging + pool + ISR 提交 | 约 1,200–1,500 行 | 3–5 人天 |
| B. 与 RT-Thread 同等完整 | A + Sem/Mutex/Cond/Thread/SoftIrq + host 测试 + 文档 | 约 2,500–3,000 行 | 2–3 周（10–15 人天） |
| C. 再加 timer/diag/coro | B + TimerScheduler + 日志后端 + 协程 | 另加 2,000 行以上 | 再加 3–5 周，coro 占大头 |

真正的风险不在 API 映射，而在三处语义差异：

1. FreeRTOS 任务函数**不能 return**，静态 TCB 不能在旧任务完全删除前复用。
2. 临界区必须区分任务上下文与 ISR 上下文，不能统一用 `taskENTER_CRITICAL()`。
3. 调度器启动模型与 RT-Thread 不同，`Runtime::start()` 的调用时机需要明确约束。

建议先做 A，验证事件管线、ISR 提交和静态内存，再补 B。timer、diag、coro 单独立项。

目标硬件限定为 **Cortex-M3/M4/M7**。Cortex-M0/M0+ 不在本方案支持范围，原因见第 11 节；这不是通过改一个原子类型就能解决的配置项。

## 1. 版本选择

“稳定版”按长期支持线解释，不采用 3 天前发布的最新补丁作为交付基线。

| 版本 | 发布时间 | 定位 | 说明 |
|---|---|---|---|
| FreeRTOS 202411.00 LTS / Kernel V11.1.0 | 2024-11-21 | **产品基线** | 202406-LTS 内核，长期修复，API 稳定 |
| FreeRTOS Kernel V11.3.3 | 2026-10-30 | 兼容验证 | 最新稳定补丁线，API 与 11.1 一致，CI 额外跑一轮 |
| FreeRTOS Kernel V10.4.3-LTS-Patch-3 | 2022-09-16 | 不纳入 | 旧 LTS，仅在厂商 SDK 无法升级时评估 |

本项目使用的 `xTaskCreateStatic`、`xSemaphoreCreate*Static`、`xSemaphoreGiveFromISR`、`xTaskNotifyGiveFromISR`、`xPortIsInsideInterrupt` 在 V11.1.0 与 V11.3.3 上均可用。V11.3.x 增加的栈溢出检查、POSIX 端口修复不影响本设计。

如果产品策略是跟随最新稳定特性线而不是 LTS，只需把产品基线换成 V11.3.3；本设计使用的 API 在两者之间没有变化。

版本来源：

- <https://github.com/FreeRTOS/FreeRTOS/releases/tag/202411.00>
- <https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V11.1.0>
- <https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V11.3.3>

## 2. 现状与可复用性

### 2.1 核心与平台层的边界

| 事实 | 证据 | 对移植的影响 |
|---|---|---|
| 核心以 `PalT` 模板参数编译期解析 | `include/coact/runtime.hpp:80`、`include/coact/dispatcher.hpp:42`、`include/coact/coordinator.hpp` 的 `DispatchCoordinator` 模板 | 不需要运行期平台分支 |
| 核心只实际调用约 12 个 PAL 入口 | `include/coact/dispatcher.hpp`、`coordinator.hpp`、`runtime.hpp` 中的 `pal_.xxx()` | 最小 PAL 面很小 |
| `current_context()` 核心未使用 | 全仓检索仅测试和 PAL 内部引用 | ISR 上下文识别不是核心必需项 |
| `register_current_task()` 核心未使用 | 仅板级代码与测试调用 | 逻辑优先级注册可选 |
| 仅 `timer.hpp` 绕过 PAL 依赖 RT-Thread | `include/coact/timer.hpp:48`、`:121` | timer 不在核心移植范围 |
| `diag/log_rtthread` 是独立后端 | `include/coact/diag/log_rtthread.hpp` | 需要单独的 FreeRTOS 后端 |
| 单核 profile 只接受 `RttSingleCoreProfile` | `include/coact/pool.hpp:57`、`:371` | FreeRTOS 侧可先 `using Profile = RttSingleCoreProfile`；长期建议重命名 |
| staging 要求 lock-free 原子 | `include/coact/staging.hpp:211` | 目标必须是 Cortex-M3/M4/M7；M0/M0+ 直接排除 |

### 2.2 现有 PAL 的体量参照

| PAL | 头文件 | 实现 | host 测试 |
|---|---|---|---|
| `pal::Posix` | 198 行 | 494 行 | 复用核心测试 |
| `pal::RtThread` | 527 行 | 807 行 | 1,200 行以上 |
| `pal::Windows` | 293 行 | 722 行 | 509 行 |

FreeRTOS PAL 与 `pal::RtThread` 最接近：都采用调用方提供的静态资源、固定槽表、零堆分配。可把它作为蓝本，而不是从 POSIX 版本改。

### 2.3 基线状态

本机 `build-baseline` 中 `test_pal_sync`、`test_rtt_pal`、`test_static_pal` 均通过，说明现有 PAL 契约测试可以作为 FreeRTOS 移植的对照基线。

## 3. 目标架构

层次关系：

| 层 | 组件 | 依赖 |
|---|---|---|
| 应用 | AO + HSM | 只通过 `Runtime` 与 `Coordinator` 接入 |
| 装配 | `Runtime` | `Dispatcher`、`Coordinator`、PAL 生命周期 |
| 执行 | `Dispatcher` / `Coordinator` | `Staging`、`ClockOps`、`wait/signal` |
| 构件 | `Staging` / `EventPool` | `CriticalSection`（`irq_save` / `irq_restore`） |
| PAL | `pal::FreeRtos` | FreeRTOS 任务、静态信号量、tick / 硬件计数器 |

## 4. 文件布局

| 文件 | 职责 |
|---|---|
| `include/coact/pal_freertos.hpp` | PAL 类型、静态资源模板、句柄定义、方法声明 |
| `src/core/pal_freertos.cpp` | FreeRTOS API 映射与生命周期实现 |
| `test/freertos_stub.h` | 可选：pthread 支撑的 host stub；若采用 POSIX 端口则不需要 |
| `src/core/test_freertos_pal.cpp` | 单核静态 PAL 契约测试 |
| `CMakeLists.txt`、`src/core/CMakeLists.txt` | 新 target 与 `COACT_FREERTOS_STUB` 编译定义 |
| `docs/interface_contract.md` | 增补 `pal::FreeRtos` 条目 |
| `README.md`、`README_zh.md` | 平台支持矩阵 |

## 5. PAL 契约映射

### 5.1 完整契约表（pal.hpp 证据）

契约来源：`include/coact/pal.hpp` 的 PAL 方法契约注释（核心入口在 `:134-153`，SemOps/MutexOps/CondOps/ThreadOps/SoftIrqOps 在 `:170-235`）；具体行为以 `include/coact/pal_rtthread.hpp`、`src/core/pal_rtthread.cpp` 和 `include/coact/pal_posix.hpp` 为准。`Posix::set_tick_hz()` 是 host 测试钩子，不属于跨 PAL 契约，未列入。

#### 5.1.1 生命周期、上下文与时钟

| 入口 | 调用上下文 | `0` 的含义 | 失败/返回 | ISR 可调用 | 证据与备注 |
|---|---|---|---|---|---|
| `initialize()` | 任务，调度器运行前后均可 | 不适用 | `InitError`：`kOk=0`、`kSemInitFailed`、`kThreadInitFailed`、`kThreadStartFailed`、`kStackTooLarge`、`kAlreadyInitialized`、`kAlreadyStarted` | 否 | `pal_rtthread.hpp:387`；创建信号量与常驻 worker；dispatcher 在 `start_dispatcher()` 创建，避免 entry/context 未设置就被调度 |
| `irq_save()` | 任务 / ISR | 不适用 | 无失败；返回 token | 是 | 任务用 `taskENTER_CRITICAL()`，ISR 用 `portSET_INTERRUPT_MASK_FROM_ISR()`；token 编码上下文 |
| `irq_restore(token)` | 与 `irq_save()` 同上下文 | 不适用 | 无 | 是 | 必须与 save 配对；`CriticalSectionGuard` 负责配对 |
| `register_current_task(prio)` | 任务，调度器运行后、`start()` 前 | 不适用 | `bool`：`false` = 表满 / 重复 tid / start 后冻结 / prio 非法 / 未初始化 / 调度器未运行 | 否 | `pal_rtthread.hpp:352`；核心不调用，板级可选；FreeRTOS 下 `xTaskGetCurrentTaskHandle()` 在调度器启动前为 NULL |
| `current_context()` | 任意 | 不适用 | 永不失败；未注册任务返回 `kind=Task`、`prio_valid=false` | 是 | `pal_rtthread.cpp:256`；核心不调用，ISR 识别用 `xPortIsInsideInterrupt()` |
| `in_dispatcher_thread()` | 任意 | 不适用 | `bool` | 是 | `pal_rtthread.hpp:364`；FreeRTOS 用 `xTaskGetCurrentTaskHandle() == dispatcher_handle_` |
| `monotonic_ns()` | 任务 / ISR | 不适用 | `0` 表示 `frequency_hz==0`（配置错误）或计数器初值为 0 | 是 | ISR 内必须用 `xTaskGetTickCountFromISR()`；板级可注入 `ClockOps` |
| `clock_resolution_ns()` | 任意 | 不适用 | `0` 仅当 `frequency_hz==0` | 是 | 等于 `1e9 / frequency_hz` |
| `set_clock_ops(ops)` | 任务，首次使用时钟前 | 不适用 | 无 | 否 | `pal.hpp:153`；可选 |
| `wait_dispatcher(ms)` | dispatcher 任务 | 永久等待 | `void`；未初始化立即返回 | 否 | `pal.hpp:143`；阻塞调用 |
| `signal_dispatcher_from_task()` | 任务 | 不适用 | `void`；未初始化返回 | 否 | `xSemaphoreGive(wake)`；多余 token 可合并 |
| `signal_dispatcher_from_isr()` | ISR | 不适用 | `void`；未初始化不得调用 | 是 | `xSemaphoreGiveFromISR` + `portYIELD_FROM_ISR`；返回 `errQUEUE_FULL` 可忽略，已有 token 保证唤醒 |
| `start_dispatcher(entry, ctx)` | 任务 | 不适用 | FreeRtos 采用 `InitError`：成功为 0；`kThreadStartFailed`、`kAlreadyStarted` 等。Posix 返回 `void` | 否 | `pal_rtthread.cpp:380`；先保存 entry/context，再 `xTaskCreateStatic` 创建 dispatcher，NULL 视为失败 |
| `join_dispatcher()` | 任务 | 永久等待 | `void`；未 started 时 no-op | 否 | `pal_rtthread.cpp:404`；`xSemaphoreTake(join, portMAX_DELAY)` |
| `watchdog_progress(marker)` | 任意 | 不适用 | `void` | 是（可选） | `pal.hpp:151`；BSP 用 `set_watchdog_ops()` 装 kick，未装时为 no-op |
| `set_dispatcher_stack_bytes(bytes)` | 任务，`initialize()` 前 | 不适用 | `void`；超容量在 `initialize()` 返回 `kStackTooLarge` | 否 | `pal.hpp:152`；FreeRTOS 需换算为 `StackType_t` 字数 |
| `dispatcher_stack_bytes()` | 任意 | 不适用 | `uint32_t` | 是 | `pal_rtthread.hpp:400`；只读 |
| `enter_direct()` / `leave_direct()` | 任务 | 不适用 | `void`；未注册任务 no-op | 否 | `pal_rtthread.hpp:403-404`；维护每线程 `direct_depth` |

#### 5.1.2 同步原语、线程与 SoftIrq

| 入口 | 调用上下文 | `0` 的含义 | 失败/返回 | ISR 可调用 | 证据与备注 |
|---|---|---|---|---|---|
| `sem_init(sem, initial)` | 任务 | 不适用 | `bool`：内核创建失败 false | 否 | `pal.hpp:171`；`xSemaphoreCreateBinaryStatic` / `xSemaphoreCreateCountingStatic` |
| `sem_take(sem, ms)` | 任务 | 非阻塞尝试 | `bool`：超时或空 false | 否 | `pal.hpp:174-175`；`kWaitForever` 才映射 `portMAX_DELAY` |
| `sem_release(sem)` | 任务 | 不适用 | `void`；计数到上限时内核返回失败，PAL 忽略或断言 | 否 | `xSemaphoreGive`；调用方不得超额释放 |
| `sem_release_from_isr(sem)` | ISR | 不适用 | `void`；计数到上限时内核返回失败，PAL 忽略或断言 | 是 | `pal.hpp:176-178`；`xSemaphoreGiveFromISR` + `portYIELD_FROM_ISR` |
| `sem_deinit(sem)` | 任务 | 不适用 | `void` | 否 | `pal.hpp:179`；`vSemaphoreDelete`，静态存储不释放 |
| `mutex_init(m)` | 任务 | 不适用 | `bool` | 否 | `pal.hpp:181-183`；`xSemaphoreCreateMutexStatic` |
| `mutex_lock(m)` | 任务 | 永久阻塞（无 timeout 参数） | `void` | 否 | `xSemaphoreTake(m, portMAX_DELAY)` |
| `mutex_unlock(m)` | 任务，持有者 | 不适用 | `void` | 否 | FreeRTOS mutex 必须由持有者释放 |
| `mutex_deinit(m)` | 任务 | 不适用 | `void` | 否 | `vSemaphoreDelete` |
| `cond_init(c)` | 任务 | 不适用 | `bool` | 否 | `pal.hpp:185-190`；计数信号量 + `waiters` 计数器 |
| `cond_wait(c, m, ms)` | 任务 | 永久等待 | `void`；超时与唤醒不可区分，调用方必须复查谓词 | 否 | 释放 mutex → 取信号量 → 重新持有 mutex |
| `cond_signal(c)` | 任务 | 不适用 | `void` | 否 | 建议在释放 mutex 后调用；否则等待方无法被唤醒 |
| `cond_broadcast(c)` | 任务 | 不适用 | `void` | 否 | 按 `waiters` 计数释放 token |
| `cond_deinit(c)` | 任务 | 不适用 | `void` | 否 | `vSemaphoreDelete` |
| `thread_create(t, entry, ctx)` | 任务 | 不适用 | `bool`：槽表耗尽或创建失败 false | 否 | `pal.hpp:192-196`；常驻 worker + `start` 信号量 |
| `thread_join(t)` | 任务 | 永久阻塞 | `void`；无效句柄 no-op | 否 | `xSemaphoreTake(join, portMAX_DELAY)` |
| `softirq_init(h)` | 任务 | 不适用 | `bool` | 否 | `pal.hpp:198-202` |
| `softirq_raise(h, payload)` | 任务 / ISR | 不适用 | `bool`：ring 满 false | 是 | 任务 `xSemaphoreGive`，ISR `xSemaphoreGiveFromISR` + yield |
| `softirq_take(h, ms)` | 任务 | 永久等待 | `int32_t`：超时 `-1` | 否 | `pal.hpp:218-221`；先取唤醒信号量，再弹出 ring |
| `softirq_deinit(h)` | 任务 | 不适用 | `void` | 否 | 清理句柄，不释放调用方存储 |
| `sleep_us(us)` | 任务 | 不适用 | `void` | 否 | `vTaskDelay`；0 等价于 yield，非 0 至少 1 tick |
| `make_critical_section(pal)` | 构造期，任意上下文 | 不适用 | 无失败；返回 `CriticalSection` | 否 | `pal.hpp:82-94`；捕获 `irq_save/restore` |
| `CriticalSectionGuard(cs)` | 任意 | 不适用 | 无；null hook 退化为 no-op | 是 | `pal.hpp:34-54`；一 save 一 restore |

### 5.2 核心必需入口

| coact 入口 | FreeRTOS V11.1.0 API | 说明 |
|---|---|---|
| `irq_save()` / `irq_restore()` | 任务：`taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`；ISR：`portSET_INTERRUPT_MASK_FROM_ISR()` / `portCLEAR_INTERRUPT_MASK_FROM_ISR()` | token 必须编码“任务/ISR + 保存值” |
| `monotonic_ns()` | `xTaskGetTickCount()`；ISR 用 `xTaskGetTickCountFromISR()`；板级可注入 `ClockOps` | 默认 tick 精度；微秒预算需硬件计数器 |
| `wait_dispatcher(ms)` | `xSemaphoreTake(wake, ticks)` | `0` 映射 `portMAX_DELAY`；非 0 至少 1 tick |
| `signal_dispatcher_from_task()` | `xSemaphoreGive(wake)` | 任务上下文 |
| `signal_dispatcher_from_isr()` | `xSemaphoreGiveFromISR(wake, &woken)` + `portYIELD_FROM_ISR(woken)` | yield 在 PAL 内部完成，不改契约 |
| `start_dispatcher(entry, ctx)` | `xTaskCreateStatic()` | 返回 `InitError`；NULL 视为失败 |
| `join_dispatcher()` | `xSemaphoreTake(join, portMAX_DELAY)` | dispatcher 退出前释放 join |
| `set_dispatcher_stack_bytes(bytes)` | 换算为 `StackType_t` 字数 | 超过资源容量返回 `kStackTooLarge` |
| `in_dispatcher_thread()` | `xTaskGetCurrentTaskHandle() == dispatcher_handle_` | 需要 `INCLUDE_xTaskGetCurrentTaskHandle=1` |
| `enter_direct()` / `leave_direct()` | 固定 `ContextSlot` 表 | 与 RT-Thread 版一致 |
| `QueueBackend` | `SingleCoreCriticalRing<T, Cap>` | 单核 irq-mask 环形队列 |
| `Profile` | `RttSingleCoreProfile` 别名 | 先复用；后续可重命名 `SingleCoreProfile` |

### 5.3 可选扩展入口

| 家族 | FreeRTOS 映射 | 关键约束 |
|---|---|---|
| SemOps | `StaticSemaphore_t` + `xSemaphoreCreateBinaryStatic` / `xSemaphoreCreateCountingStatic` | `configUSE_COUNTING_SEMAPHORES=1` |
| MutexOps | `xSemaphoreCreateMutexStatic` | `configUSE_MUTEXES=1`；必须由持有者释放 |
| CondOps | 计数信号量 + `waiters` 计数器，模拟 `pthread_cond_wait` | 信号方不得持锁；与 RT-Thread 版同一纪律 |
| ThreadOps | 常驻 worker 任务 + `start`/`join` 信号量 | 避免静态 TCB 删除/复用竞争 |
| SoftIrqOps | SPSC ring + 二值信号量或任务通知 | 比 RT-Thread 的信号方案更简单 |
| `sleep_us()` | `vTaskDelay()`，非 0 至少 1 tick | `INCLUDE_vTaskDelay=1` |

### 5.4 超时语义

coact 有两套约定，FreeRTOS 侧不能混用：

| 入口 | `0` 的含义 | 映射 |
|---|---|---|
| `wait_dispatcher(0)` | 永久等待 | `portMAX_DELAY` |
| `cond_wait(..., 0)` | 永久等待 | `portMAX_DELAY` |
| `softirq_take(..., 0)` | 永久等待 | `portMAX_DELAY` |
| `sem_take(..., 0)` | 非阻塞尝试 | `0` |
| `mutex_lock()` | 不适用（无 timeout 参数） | `portMAX_DELAY` |

非 0 毫秒换算必须向上取整并至少 1 tick，不能直接使用 `pdMS_TO_TICKS`。例如 `configTICK_RATE_HZ=100` 时，`kBatchTimeoutMs=5` 会被 `pdMS_TO_TICKS` 算成 0，退化为忙轮询。

### 5.5 静态资源模板

与 `RtThreadResources` 对应，FreeRTOS 侧的资源由调用方持有。建议签名：

```cpp
template <uint32_t DispatcherStackBytes,
          uint16_t ContextSlots,
          uint16_t WorkerSlots = 0U,
          uint32_t WorkerStackBytes = 1024U>
struct FreeRtosResources;
```

内部成员：

| 成员 | 类型 | 用途 |
|---|---|---|
| `dispatcher_stack` | `StackType_t[words]` | dispatcher 静态栈，按 `StackType_t` 字数计算 |
| `dispatcher_tcb` | `StaticTask_t` | dispatcher 静态 TCB |
| `wake` / `join` | `StaticSemaphore_t` | dispatcher 唤醒与退出信号量 |
| `slots` | `ContextSlot[ContextSlots]` | 生产者逻辑优先级表，按 `TaskHandle_t` 索引 |
| `worker_tcb` / `worker_stack` | `StaticTask_t[]` / `StackType_t[]` | 常驻 worker 的静态 TCB 与栈 |
| `worker_start` / `worker_join` | `StaticSemaphore_t[]` | 每个 worker 槽的启动与完成信号量 |

构造函数只保存引用并完成值初始化，不调用 FreeRTOS API；`initialize()` 在任务上下文创建信号量与常驻 worker，`start_dispatcher()` 再创建 dispatcher，任何失败返回确定的 `InitError`。

## 6. 关键设计决策

### D1：临界区按上下文分支

`try_submit_from_isr()` 会把事件推进 `SingleCoreCriticalRing`，因此 `CriticalSection::save()` 必须同时支持任务和 ISR。`taskENTER_CRITICAL()` 不能在 ISR 调用，ISR 必须用 `portSET_INTERRUPT_MASK_FROM_ISR()`。

实现方式：

```cpp
CriticalToken irq_save() noexcept {
    if (coact_freertos_in_isr()) {
        const UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
        return CriticalToken{(static_cast<uintptr_t>(mask) << 1) | 1U};
    }
    taskENTER_CRITICAL();
    return CriticalToken{0U};
}

void irq_restore(CriticalToken token) noexcept {
    if ((token.value & 1U) != 0U) {
        portCLEAR_INTERRUPT_MASK_FROM_ISR(token.value >> 1);
    } else {
        taskEXIT_CRITICAL();
    }
}
```

`coact_freertos_in_isr()` 的选择：

- Cortex-M3/M4/M7：`xPortIsInsideInterrupt() != pdFALSE`。
- Cortex-M0/M0+：`xPortIsInsideInterrupt()` 可用（V11.1.0 的 ARM_CM0 端口声明），但该内核不在本方案支持范围，见第 11 节。
- POSIX 端口：没有该 API，host 测试用 `COACT_FREERTOS_IN_ISR_HOOK` 注入。
- 其他端口：要求集成方定义 `COACT_FREERTOS_IN_ISR_HOOK()`；否则编译期报错，不静默退化为“永远任务上下文”。

注意：Cortex-M3/M4/M7 的 `taskENTER_CRITICAL()` 与 `portSET_INTERRUPT_MASK_FROM_ISR()` 都只屏蔽数值大于等于 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的中断，不是 PRIMASK 全屏蔽。因此所有调用 coact 的 ISR 必须满足 FreeRTOS 的 syscall 优先级约束；高于该阈值的中断不得调用 `try_submit_from_isr()`。若产品确实需要更高优先级 ISR 参与提交，只能在 PAL 内改用 PRIMASK 方案并单独验证对 tick/PendSV 的影响。Cortex-M0/M0+ 没有 BASEPRI，端口实现本身就是 PRIMASK 全屏蔽。

### D2：ISR 唤醒在 PAL 内完成 yield

```cpp
void signal_dispatcher_from_isr() noexcept {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(wake_, &woken);
    portYIELD_FROM_ISR(woken);
}
```

契约不变，调用方仍只调用无参 `signal_dispatcher_from_isr()`。注意调用该函数的 ISR 优先级必须不高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`（数值上大于等于），否则 FreeRTOS 会断言或破坏内核状态。

### D3：worker 线程采用常驻任务

FreeRTOS 任务函数不能 return；返回会进入 `prvTaskExitError` 或未定义行为。RT-Thread 版“entry 返回后释放 join”的写法不能直接照搬。

推荐模型：

```cpp
void worker_entry(void* p) {
    auto* slot = static_cast<WorkerSlot*>(p);
    for (;;) {
        xSemaphoreTake(slot->start, portMAX_DELAY);
        slot->handle->entry(slot->handle->context);
        xSemaphoreGive(slot->join);
    }
}
```

- `initialize()` 时为每个 worker 槽创建常驻任务。
- `thread_create()` 只填写句柄并 `xSemaphoreGive(start)`。
- `thread_join()` 只 `xSemaphoreTake(join)`，不删除 TCB。
- 代价是 `WorkerSlots` 个任务和栈常驻；这是静态内存设计的自然结果。

不推荐按需 `xTaskCreateStatic` + `vTaskDelete(NULL)` 后立即复用同一 TCB：join 信号在任务真正进入终止链表之前释放，复用会与空闲任务的清理竞争。

### D4：任务函数显式删除

dispatcher 与 worker 的 trampoline 都不得以 `return` 结束。dispatcher 退出路径：

```cpp
user_entry_(user_ctx_);
xSemaphoreGive(join_);
vTaskDelete(NULL);
```

dispatcher 不支持重启，因此静态 TCB 不需要复用，删除后无需等待空闲任务清理。

### D5：优先级映射

FreeRTOS 数值越大优先级越高，`0` 通常留给 idle。coact 的 `LogicalPrio` 是 `1..63`，数值越大越高。

建议：

- 静态断言 `configMAX_PRIORITIES >= 2`。
- dispatcher 使用 `configMAX_PRIORITIES - 1`，或由 `FreeRtosResources` 显式配置。
- 生产者映射为 `min(max(logical, 1), configMAX_PRIORITIES - 2)`；最高应用优先级留给 dispatcher，避免生产者与唯一排空 staging 的线程同优先级。
- 不允许把 `LogicalPrio` 直接当 `UBaseType_t` 传入；RT-Thread 版硬编码的 `10` 在 `configMAX_PRIORITIES=7` 的板子上非法。

### D6：时钟

默认 `ClockOps` 使用 `xTaskGetTickCount()`：

```cpp
ops.read_counter = &tick_counter_read;
ops.frequency_hz = configTICK_RATE_HZ;
ops.counter_bits = 32U;
```

- `configTICK_RATE_HZ=1000` 时分辨率为 1 ms，满足 `kBatchTimeoutMs=5` 与 RTC 预算 1 ms。
- `kDirectBudgetNs=50 us` 需要板级注入硬件计数器（Cortex-M3+ 可用 DWT CYCCNT，或 TIM）。
- `monotonic_ns()` 若在 ISR 调用，必须使用 `xTaskGetTickCountFromISR()`；实现内用同一个上下文判断分支。

### D7：SoftIrq 不使用信号

FreeRTOS 没有 POSIX 信号。RT-Thread 版的 `rt_signal_install` / `rt_thread_kill` 全部删除。

推荐：

- `SoftIrqHandle` 内嵌 SPSC ring（8 槽）+ `StaticSemaphore_t` 二值信号量。
- `softirq_raise()` 写入 ring 后 `xSemaphoreGiveFromISR()`（或任务上下文 `xSemaphoreGive()`）。
- `softirq_take()` 先 `xSemaphoreTake(wake, ticks)`，再弹出 ring；超时返回 `-1`。
- 也可以使用 `xTaskNotifyGiveFromISR()` + `ulTaskNotifyTake()`，更轻量；二值信号量方案与现有 `SoftIrqOps` 语义更接近，二选一即可。

### D8：Profile 与静态断言

短期：

```cpp
using Profile = coact::RttSingleCoreProfile;
```

`EventPool` 当前只接受 `RttSingleCoreProfile` / `HostSmpProfile`，见 `include/coact/pool.hpp:371`。别名可以零改动复用。

长期建议把 `RttSingleCoreProfile` 重命名为 `SingleCoreProfile`，保留旧名别名，并让 `detail::is_single_core_pool_profile_v` 接受新类型。改动约 4 个文件、10 行，属于独立的小重构。

### D9：调度器启动模型

FreeRTOS 的调度器由应用调用 `vTaskStartScheduler()` 启动，且该函数通常不返回；RT-Thread 则由内核启动后在任务中创建线程。`Runtime::start()` 的语义必须按 FreeRTOS 的方式明确。

推荐启动序列：

```cpp
static FreeRtosResources<4096U, 8U, 4U, 1024U> res;
static coact::pal::FreeRtos pal(res);
static coact::Runtime<BoardCfg, coact::pal::FreeRtos,
                      coact::pal::FreeRtos::Profile> rt(pal);

void bootstrap_task(void*)
{
    rt.bind(&ao_a);
    rt.bind(&ao_b);
    rt.initialize();
    rt.start();                 /* 创建 dispatcher 任务；本任务在调度器运行后调用 */
    vTaskSuspend(NULL);         /* 永久阻塞；不要用 vTaskDelay(portMAX_DELAY) */
}

int main()
{
    static StackType_t boot_stack[512];
    static StaticTask_t boot_tcb;
    xTaskCreateStatic(bootstrap_task, "boot",
                      sizeof(boot_stack) / sizeof(StackType_t),
                      nullptr, 1U, boot_stack, &boot_tcb);
    vTaskStartScheduler();      /* 不返回 */
}
```

约束：

- `bootstrap_task` 本身在调度器启动后才运行，因此 `rt.start()` 满足“调度器运行后”的约束；`main()` 只负责创建 bootstrap 任务并启动调度器，不调用 `rt.start()`。
- 在 `vTaskStartScheduler()` 之前调用 `rt.start()` 虽然能创建 dispatcher 任务，但 `stop()` 的 join 会永久阻塞；该顺序不支持。
- `Runtime::stop()` 也只能在调度器运行后调用。
- 如果产品必须在 `main()` 里先 `start()` 再启动调度器，需要把 `join_dispatcher()` 改成非阻塞轮询或在文档中明确禁止该顺序。
- `vTaskDelay(portMAX_DELAY)` **不是**永久阻塞。V11.1.0 的 `vTaskDelay()` 以 `xCanBlockIndefinitely=pdFALSE` 调用 `prvAddCurrentTaskToDelayedList()`，`portMAX_DELAY` 只表示约 `0xFFFFFFFF` tick 的有限延迟，tick 溢出后可能被唤醒。永久阻塞用 `vTaskSuspend(NULL)`（需要 `INCLUDE_vTaskSuspend=1`），或等待一个永不释放的信号量。

生命周期 API（`initialize` / `start_dispatcher` / `join_dispatcher`）不做内部同步，必须由同一任务按顺序调用；生产者与 ISR 侧入口可以并发。dispatcher 身份保存在 `FreeRtos` 实例内，而不是翻译单元全局量，因此同一镜像里的两个 PAL 实例不会互相覆盖身份；`in_dispatcher_thread()` 也相应改为实例方法。

`Dispatcher::in_dispatcher_thread()` 随 PAL 一起改为实例方法，转发到构造时持有的 PAL 引用。cmdfw 这类需要把“是否在 dispatcher 线程”注册成回调的调用方，必须绑定 `[&d] { return d.in_dispatcher_thread(); }`，或直接使用自己的 PAL 实例；`DispatcherT::in_dispatcher_thread()` 的静态写法不再成立。

## 7. FreeRTOSConfig.h 要求

| 配置项 | 值 | 原因 |
|---|---|---|
| `configNUMBER_OF_CORES` | `1` | coact 单核 profile；多核直接 `#error` |
| `configSUPPORT_STATIC_ALLOCATION` | `1` | 所有任务/信号量静态分配 |
| `configSUPPORT_DYNAMIC_ALLOCATION` | `0`（推荐） | 零堆目标；若 BSP 需要可保留 1 |
| `configUSE_MUTEXES` | `1` | MutexOps |
| `configUSE_COUNTING_SEMAPHORES` | `1` | SemOps / CondOps |
| `configUSE_TASK_NOTIFICATIONS` | `1`（默认） | SoftIrq 可选方案 |
| `INCLUDE_vTaskDelay` | `1` | `sleep_us()` |
| `INCLUDE_vTaskPrioritySet` | `1` | `register_current_task()` 把逻辑优先级映射到原生优先级；缺失时无法满足优先级契约 |
| `INCLUDE_xTaskGetCurrentTaskHandle` | `1`（默认） | `in_dispatcher_thread()` |
| `INCLUDE_vTaskSuspend` | `1` | `xSemaphoreTake(..., portMAX_DELAY)` 与 `vTaskSuspend(NULL)` 才是永久阻塞；`vTaskDelay(portMAX_DELAY)` 仍不是 |
| `INCLUDE_vTaskDelay` | `1` | `sleep_us()` |
| `INCLUDE_xTaskGetSchedulerState` | `1` | `start_dispatcher()` 拒绝调度器启动前的调用 |
| `INCLUDE_xTaskGetCurrentTaskHandle` | `1` | 上下文表键与 dispatcher 身份比较 |
| `INCLUDE_vTaskDelete` | `1` | dispatcher 退出后删除自身 TCB |
| `configTICK_TYPE_WIDTH_IN_BITS` | `TICK_TYPE_WIDTH_32_BITS`（旧配置可用 `configUSE_16_BIT_TICKS=0`） | 32 位 tick；16 位不满足老化/超时 |
| `configMAX_PRIORITIES` | 至少 `3`（idle、生产者、dispatcher），建议 `>= 8` | dispatcher 与生产者优先级 |
| `configTICK_RATE_HZ` | 建议 `1000` | 5 ms 批次超时与 1 ms RTC 预算 |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | 按 MCU 设置 | ISR 调用 `...FromISR` 的前提 |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 板级栈水位验证 |
| `configASSERT` | 必须定义 | 开发期捕获优先级/静态分配误用 |
| `vApplicationGetIdleTaskMemory` | 必须提供 | 静态分配开启后的 idle 任务内存 |
| `vApplicationGetTimerTaskMemory` | `configUSE_TIMERS=1` 时提供 | 静态分配下的 timer 任务内存 |

## 8. 实施计划

| 阶段 | 交付物 | 估算 | 验证 |
|---|---|---|---|
| P0 版本与配置冻结 | `FreeRTOSConfig.h` 基线、目标芯片/端口确认 | 0.5 人天 | 配置能编译最小 demo |
| P1 核心 PAL | `pal_freertos.hpp` / `.cpp`，核心 12 个入口 | 3–4 人天 | 单 Dispatcher + 2 AO 跑通 |
| P2 host 测试 | FreeRTOS POSIX 端口或 stub、`test_freertos_pal.cpp` | 2–3 人天 | 与 `test_static_pal` / `test_rtt_pal` 对齐 |
| P3 板级 bring-up | 真实中断提交、栈水位、长稳 | 3–4 人天 | ISR 延迟、24h soak、零堆 |
| P4 文档与 CI | 契约、README、CI matrix | 1 人天 | LTS + V11.3.3 双版本构建 |

表中合计约 10–13 人天；计入板级反复与不可预见项后按 12–15 人天估算，即 2.5–3 周。只做 A 档可压缩到 3–5 人天。

### 8.1 实施顺序

1. 冻结 `FreeRTOSConfig.h` 与目标端口，确认 `xPortIsInsideInterrupt` 和 `portSET_INTERRUPT_MASK_FROM_ISR` 可用。
2. 新增 `pal_freertos.hpp/.cpp`，先只实现核心 12 个入口和 `SingleCoreCriticalRing` 后端。
3. 用 FreeRTOS POSIX 端口在 Linux 上跑通 `test_freertos_pal`，再补板级 demo。
4. 补 Sem/Mutex/Cond/Thread/SoftIrq，复用 `test_pal_sync` 的用例结构。
5. 最后补文档、CI 和 `interface_contract.md`。

## 9. 测试策略

### 9.1 host

优先使用 FreeRTOS 官方 POSIX 端口（`portable/ThirdParty/GCC/Posix`），直接跑真实内核，比手写 stub 更能暴露调度语义。代价是 CI 需要引入 FreeRTOS-Kernel 源码（submodule 或固定版本压缩包）。

备选：沿用 `test/rtthread_stub.h` 的模式，写一个 pthread 支撑的 `test/freertos_stub.h`。优点是零外部依赖；缺点是无法完全模拟 `xPortIsInsideInterrupt`、静态 TCB 删除和 `portYIELD_FROM_ISR`。

### 9.2 契约测试

把 `test_static_pal.cpp` 与 `test_rtt_pal.cpp` 中与平台无关的用例抽成“单核静态 PAL 契约测试”，同时实例化 `RtThread` 和 `FreeRtos`。这样 FreeRTOS 不需要复制 1,200 行测试，也能防止两个后端行为漂移。

### 9.3 板级

| 项目 | 方法 | 通过标准 |
|---|---|---|
| ISR 提交 | 真实定时器中断调用 `try_submit_from_isr()` | 无丢失、无断言、延迟在预算内 |
| 唤醒延迟 | 示波器或 GPIO 打点 | 从 ISR 到 dispatcher 开始派发的延迟可测 |
| 栈水位 | `uxTaskGetStackHighWaterMark()` | dispatcher / worker 余量 >= 25% |
| 零堆 | map 文件、`elf_audit.sh` | 无 `pvPortMalloc` / `malloc` 引用 |
| 长稳 | 24h 连续提交/派发 | 无泄漏、无优先级反转、无看门狗复位 |
| 静态断言 | 编译期 | `atomic<bool>` / `atomic<uint16_t>` / `atomic<uint32_t>` lock-free |

### 9.4 CI

- Linux host：FreeRTOS POSIX 端口，跑核心与 PAL 测试。
- 交叉编译：至少一个 Cortex-M3/M4 目标，验证 `-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic`。
- 双版本：Kernel V11.1.0 LTS 与 V11.3.3 各构建一次。

## 10. 风险登记

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 静态 TCB 复用竞争 | 中 | 高 | 采用常驻 worker，不做 TCB 复用 |
| 临界区在 ISR 误用 `taskENTER_CRITICAL` | 中 | 高 | 上下文分支 + 编译期 hook 必填 |
| 高优先级 ISR 越过 syscall 阈值提交 | 中 | 高 | 文档约束 + `configASSERT`；必要时改用 PRIMASK 并单独验证 |
| `configMAX_PRIORITIES` 不足 | 中 | 中 | 编译期断言 + 文档 |
| `configSUPPORT_STATIC_ALLOCATION=0` | 中 | 中 | 头文件 `#error` + 移植文档 |
| 100 Hz tick 导致超时归零 | 中 | 中 | 向上取整 + 至少 1 tick |
| POSIX 端口缺少 `xPortIsInsideInterrupt` | 高 | 低 | host 测试注入 `COACT_FREERTOS_IN_ISR_HOOK` |
| 调度器启动时机错误 | 中 | 中 | 明确 boot sequence，`start()` 后由应用调用 `vTaskStartScheduler()` |
| timer 的 `std::mutex` / `<pthread.h>` 不可用 | 高 | 中 | timer 单独立项，先只支持 `ManualTickSource` + 板级线程 |
| coro 的 `ucontext` 不可用 | 高 | 高 | 明确不纳入首轮；需要新的上下文切换后端 |

## 11. 不适用范围

- **coro**：`include/coact/coro/posix.hpp` 基于 `ucontext`，Cortex-M 裸机不可用。需要 setjmp/longjmp 或汇编上下文切换、独立栈池和板级验证，属于独立项目；RT-Thread 侧的协程完整性在契约中也标注为未验证。
- **TimerScheduler 完整移植**：`include/coact/timer.hpp:48` 无条件包含 `<pthread.h>`，`:493` 使用 `std::mutex`，线程路径使用 `pthread_create`。FreeRTOS 上需要锁/线程策略抽象，约 2–3 人天。
- **diag 日志后端**：`log_rtthread.hpp/.cpp` 是 RT-Thread 专用；FreeRTOS 后端约 300 行、1–2 人天。
- **SMP**：`configNUMBER_OF_CORES > 1` 直接 `#error`，与现有 `pal::RtThread` 的单核约束一致。
- **Cortex-M0/M0+ 不支持**：`staging.hpp:211` 静态断言 `std::atomic<bool>::is_always_lock_free`，`spsc_ring.hpp:61` 静态断言 `std::atomic<uint16_t>::is_always_lock_free`。ARMv6-M 没有字节/半字独占访问指令，这两个断言在 M0/M0+ 上通常直接失败。把字段改成 32 位原子在 ARMv6-M 上可能可行，但这是对 `staging` / `SpscRing` 的核心改动，需要重新验证内存序、ABI、单核 profile 和工具链行为，不能作为本 PAL 移植的配置项。若产品目标确实是 M0/M0+，应另立内核兼容性项目，重新评估原子、临界区和 SpscRing。

## 12. 待确认事项

1. 目标芯片是 Cortex-M3/M4/M7 中的哪一个，以及使用哪个 FreeRTOS 官方端口或厂商端口。若目标为 M0/M0+，本方案不适用，需另立项。
2. 产品是否允许 `configSUPPORT_DYNAMIC_ALLOCATION=0`，以及 BSP/中间件是否依赖动态分配。
3. “支持”的范围：只要核心事件框架，还是包含 timer、diag、coro。
4. 是否接受把 `RttSingleCoreProfile` 重命名为 `SingleCoreProfile`（保留旧名别名）。
5. 板级验证资源：是否有真实目标板或 QEMU 环境；CI 是否允许引入 FreeRTOS-Kernel 源码。

## 13. 实现状态与验证

已落地文件：

| 文件 | 内容 |
|---|---|
| `include/coact/pal_freertos.hpp` | `FreeRtosResources`、`ContextSlot`、`WorkerSlot`、`FreeRtos` 类与编译期配置门禁 |
| `src/core/pal_freertos.cpp` | 上下文分支临界区、ClockOps、dispatcher/worker 生命周期、Sem/Mutex/Cond/Thread/SoftIrq |
| `test/freertos_stub.h` | pthread 支撑的 FreeRTOS API 子集，含故障注入与 ISR 模拟 |
| `src/core/test_freertos_pal.cpp` | 22 组 host 契约测试（资源布局、初始化失败、启动/join、调度器未运行、双实例身份、ContextSlot 冻结/满表、优先级映射、ISR、嵌套临界区、时钟扩展、Sem/Mutex/Cond/signal+broadcast/timeout、sleep、worker 复用与并发 create/join、SoftIrq、Runtime 端到端） |
| `test/renode/freertos_wdg_main.cpp` | 故障夹具：IWDG 终止关中断死循环、MemManage 现场记录与复位、configASSERT 复位，跨 4 次启动用 `.noinit` 记账 |

实测结果（2026-11-02，本机）：

- `ctest --test-dir build-v2`：58/58 通过。
- `test_freertos_pal`：22 组、93 个断言全部通过。
- Renode（Cortex-M4 模型，真实 FreeRTOS V11.1.0 CM3 端口）：`test/renode/freertos.sh` 32/32 通过；覆盖 SVC/PendSV 调度、SysTick 推进、TIM2 真实中断进入 ISR 分支、Sem/Mutex、常驻 worker、双任务并发 create/join 与 Runtime 端到端。`configCHECK_FOR_STACK_OVERFLOW=2`，dispatcher 栈水位 977/1024 字余量（断言要求 >= 25%）。
- Renode 故障夹具：`test/renode/freertos_wdg.sh` 8/8 通过。IWDG 在中断关闭、调度器停转的条件下于 **29.862 ms** 复位（窗口 32 ms）；MemManage 记录 `CFSR=0x82`、`MMFAR=0x20020000` 后复位；`configASSERT` 走同一复位路径。
- `__atomic_compare_exchange_8` 调用点为 0；生产 PAL 的 AUTOSAR/CERT 检查 0 告警，MISRA C++ 范围与偏差见 `docs/cpp_coding_conventions_zh.md`。
- 编译门禁：`configNUMBER_OF_CORES==1`、`configSUPPORT_STATIC_ALLOCATION==1`、mutex/counting semaphore、`INCLUDE_vTaskSuspend==1`、`INCLUDE_vTaskDelay==1`、`INCLUDE_xTaskGetSchedulerState==1`、32 位 tick、`configMAX_PRIORITIES>=3` 均为 `#error` 级检查；Cortex-M0/M0+ 与 SMP 直接拒绝。

边界：

- host stub 不按优先级调度，不产生真实 tick 中断，ISR 是同步调用的；它只能证明 PAL 的协议与控件选择，不能替代 P3 板上验证。
- Renode 夹具使用 CM3 端口 + 真实内核，已覆盖真实链接、SysTick 与 TIM2 中断；仍不覆盖 CM4F/FPU 端口、厂商 BSP、`xPortIsInsideInterrupt` 在目标端口的可见性、tickless/低功耗、栈水位与 24h 长稳。
- timer、diag、coro 按第 11 节不纳入本次实现。

### 13.1 安全契约与故障处置矩阵

安全状态由板级定义：PAL 不掌握输出，也不把 Breaker 的 `Safe` 当成硬件安全状态。默认安全动作是**系统级复位循环**，由 BSP 在复位前把执行器置安全；对“必须继续运行”的故障只允许软件降级。

故障反应时限由板级需求给出。夹具取 IWDG 32 ms 窗口，实测 29.862 ms；这是该模型上的证据，不是产品时限承诺。

| 故障 | 检测 | 动作 | 证据 |
|---|---|---|---|
| 准入拒绝、队列满、Breaker L1/L2 | 软件计数/状态机 | 拒绝或降级，继续运行 | admission 夹具 S1–S14 |
| 处理函数不返回、关中断死循环 | 独立 IWDG | 复位 | `freertos_wdg.sh`：关中断后 29.862 ms 复位 |
| `configASSERT` 失败 | FreeRTOS 断言钩子 | 记录后复位 | `freertos_wdg.sh` 第 3 次启动 |
| 栈溢出 | `configCHECK_FOR_STACK_OVERFLOW=2` | 钩子记录后复位 | 主夹具编译并链接钩子；dispatcher 水位 977/1024 字 |
| MPU/内存属性违规 | MemManage | 记录 CFSR/MMFAR 后复位 | `CFSR=0x82`、`MMFAR=0x20020000` |
| 硬故障 | HardFault | 记录 CFSR/MMFAR 后复位 | 钩子已链接；MemManage 用例覆盖同一复位路径 |

不允许的处置：断言后继续执行、栈溢出后返回、Breaker 置 `Safe` 就宣称硬件已安全、用软件预算终止不返回的处理函数。

### 13.2 AUTOSAR 风格钩子

coact 核心不提供 `ErrorHook` / `ProtectionHook` / `PanicHook`。需要单一入口的产品在 BSP
适配层实现 `PalHooks`，示例见 `examples/pal_hooks_demo.cpp`：

| 钩子 | 触发点 | 适配方式 |
|---|---|---|
| `on_error` | coact API 返回失败 | BSP 包装返回值检查；返回值仍是唯一权威，可在此转发产品 FCCU |
| `on_protection` | `MemManage_Handler` / `HardFault_Handler` | BSP 保存 CFSR/MMFAR 后调用钩子，再执行复位 |
| `on_panic` | 框架不变量破坏 | 通过 `coact::set_fatal_handler()` 安装；钩子返回后 `fatal_assert()` 仍会 `abort()` |

把三个钩子放在 BSP 而不是 `include/coact`，可以避免热路径多出分支，也不会让中间件
假装拥有 OS 的保护域语义。

示例在 BSP 侧使用 `FirstFaultLatch` 锁存首故障：第一条错误、保护异常或 panic
保留原始类型、位置和寄存器/原因码，后续故障不得覆盖。它只服务硬故障临界路径，
不排队、不分配、不启动消费线程，也不替代 FCCU。普通软件错误由 `on_error` 转发给
产品 FCCU；保护异常和 panic 不经过 FCCU 消费队列。钩子内部再次触发故障时，
适配层只增加重入计数并返回最小安全处置路径，不再递归调用同一组钩子。
首故障锁存和重入状态属于 BSP，不进入 coact 核心。

## 14. 参考

- coact PAL 契约：`include/coact/pal.hpp`
- RT-Thread PAL 蓝本：`include/coact/pal_rtthread.hpp`、`src/core/pal_rtthread.cpp`
- 单核队列：`include/coact/queue.hpp` 的 `SingleCoreCriticalRing`
- 静态资源与错误传播：`src/core/test_static_pal.cpp`
- FreeRTOS LTS 发布说明：<https://github.com/FreeRTOS/FreeRTOS/releases/tag/202411.00>
- FreeRTOS Kernel V11.1.0：<https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V11.1.0>
