# ISP Pipeline 静态 AOP 增强方案

## 结论先行

`isp_pipeline` 已经具备 CRTP worker 分层，但当前分层主要解决“代码复用”，还没有系统性承载计时、Trace、Fault 和生命周期切面。下一步建议采用 **CRTP + 编译期策略链** 做静态 AOP，不引入运行时 aspect 注册表、虚函数、`std::function` 或动态分配。

目标结构：

```text
WorkerRuntime
  -> Lifecycle / SPSC / Wake
  -> TraceAspect
  -> MetricsAspect
  -> FaultAspect
  -> CompletionAspect / PeriodicAspect / SoftIrqAspect
  -> concrete worker hook
```

这里的 aspect 是编译期策略，不是运行时对象。每个 worker 只编译出自己实际使用的切面。

## 1. 当前基础

| 组件 | 当前实现 | 可增强点 |
|---|---|---|
| 基础交接 | `WorkerBase<Derived, Job, Depth, PalT>` | 统一生命周期、SPSC、唤醒、stop drain |
| 完成型 worker | `CompletionWorkerBase` | 统一完成事件分配、回投和失败计数 |
| 周期 producer | `PeriodicProducerBase` | 统一周期、停止和产帧计数 |
| SoftIrq worker | `SoftIrqCompletionWorker` | 统一 mailbox、消费者和收尾统计 |
| 具体 worker | `IspIrqWorker`、`SoutDmaWorker`、`MipiIrqWorker`、`CmdDmaWorker`、`IrscWorker`、`UsbDmaWorker` | 保留硬件时序、协议字段和异常语义 |

当前分层位置：

- `WorkerBase`、`PeriodicProducerBase`、`SoftIrqCompletionWorker`、`CompletionWorkerBase`：`examples/isp_pipeline/sensor_irsc.hpp`
- 完成型 worker：`examples/isp_pipeline/isp_chain.hpp` 与 `sensor_irsc.hpp`
- RT-Thread PAL：`include/coact/pal_rtthread.hpp`

## 2. 为什么不用简单的外层继承

当前 `WorkerBase` 通过 `static_cast<Derived*>(this)` 静态调用 `execute()`。如果直接把 `TraceAspect<CompletionWorkerBase<...>>` 包在外面，`WorkerBase::run()` 仍会静态调用内部的 `CompletionWorkerBase::execute()`，外层 Trace aspect 可能被绕过。

因此不能只靠下面这种写法扩展现有层次：

```cpp
using Worker = TraceAspect<CompletionWorkerBase<Concrete, Job, Depth>>;
```

正确方向是让 `WorkerBase` 的执行入口显式接受一个编译期 `InvokePolicy`，由策略链完成环绕调用；CRTP 负责最终 worker 身份，策略链负责静态织入。

## 3. 推荐结构：CRTP worker + 策略链

### 3.1 统一执行入口

```cpp
enum class WorkerResult : uint8_t {
    kOk,
    kRejected,
    kTimeout,
    kFault
};
```

```cpp
template <typename Derived, typename Job, uint16_t Capacity,
          typename InvokePolicy>
class WorkerRuntime {
public:
    WorkerResult run_job(const Job& job) noexcept
    {
        return InvokePolicy::invoke(*static_cast<Derived*>(this), job);
    }
};
```

`WorkerRuntime` 只负责 CRTP 下转和调用策略，不知道 Trace、Fault 或完成事件的具体实现。

### 3.2 最底层业务策略

```cpp
struct CompletionInvoke {
    template <typename Worker, typename Job>
    static WorkerResult invoke(Worker& worker, const Job& job) noexcept
    {
        return worker.execute_job(job);
    }
};
```

具体 worker 只实现 `execute_job()`，不再重复实现队列、线程和完成事件模板代码。

### 3.3 计时切面

```cpp
template <typename Next>
struct MetricsAspect {
    template <typename Worker, typename Job>
    static WorkerResult invoke(Worker& worker, const Job& job) noexcept
    {
        const uint64_t start_ns = worker.monotonic_ns();
        const WorkerResult result = Next::invoke(worker, job);
        worker.add_execution_duration(worker.monotonic_ns() - start_ns);
        return result;
    }
};
```

`monotonic_ns()` 必须来自 PAL，不能在 aspect 中直接调用 `clock_gettime()`。RT-Thread 版本使用 PAL 注入的时钟，host 版本使用 POSIX PAL。

### 3.4 Trace 切面

```cpp
template <typename Next>
struct TraceAspect {
    template <typename Worker, typename Job>
    static WorkerResult invoke(Worker& worker, const Job& job) noexcept
    {
        worker.trace_before(job);
        const WorkerResult result = Next::invoke(worker, job);
        worker.trace_after(job);
        return result;
    }
};
```

Trace aspect 只传递固定宽度字段：worker id、job/frame id、path、elapsed 和结果码。禁止读取 payload、打印字符串或持有事件指针。

### 3.5 Fault 切面

coact 无异常，因此 Fault aspect 不做 `try/catch`，而是要求业务 hook 返回显式结果：

```cpp
template <typename Next>
struct FaultAspect {
    template <typename Worker, typename Job>
    static WorkerResult invoke(Worker& worker, const Job& job) noexcept
    {
        const WorkerResult result = Next::invoke(worker, job);
        if (result != WorkerResult::kOk) {
            worker.report_fault(result, job);
        }
        return result;
    }
};
```

Fault sink 使用 `FaultReporter`，并区分 task-safe 与 ISR-safe 回调。故障上报失败不能阻塞原 worker，也不能改变事件所有权。

### 3.6 策略组合顺序

建议按以下顺序组合：

```cpp
using IspInvoke =
    TraceAspect<
        MetricsAspect<
            FaultAspect<
                CompletionInvoke>>>;
```

调用顺序：

```text
Trace before
  -> start timing
    -> execute job
    -> explicit fault check
  -> stop timing
Trace after
```

组合顺序必须固定在类型别名中，不提供运行时调整入口。

## 4. 三类 worker 的织入方式

### 4.1 完成型 worker

`IspIrqWorker`、`SoutDmaWorker`、`MipiIrqWorker` 和 `CmdDmaWorker` 使用：

```text
WorkerRuntime
  -> SPSC/Lifecycle/Wake
  -> TraceAspect
  -> MetricsAspect
  -> FaultAspect
  -> CompletionAspect
  -> execute_job()
```

Completion aspect 统一：

- `alloc_typed()` 完成事件；
- 填写 `IoMeta`；
- `submit_from_task()` 或 `try_submit_from_isr()`；
- completion allocation reject；
- elapsed 和 timeout；
- completion event 的 disposition。

不同 worker 只提供：

- 完成 signal；
- job 到 `IoMeta` 的映射；
- 硬件模拟延迟；
- 目标 AO。

### 4.2 周期 producer

`IrscWorker` 使用：

```text
PeriodicProducerRuntime
  -> Thread lifecycle
  -> PeriodicAspect
  -> Trace/Metrics/Fault
  -> produce_frame()
```

它不使用 SPSC job ring，也不继承 Completion aspect。周期节拍仍由 PAL `sleep_us()` 提供，不能用 Fault/Trace aspect 代替节拍控制。

### 4.3 SoftIrq completion worker

`UsbDmaWorker` 使用组合能力：

```text
UsbDmaRuntime
  -> single-slot DMA handoff
  -> SoftIrqAspect
  -> Trace/Metrics/Fault
  -> DMA transaction hook
```

SoftIrq aspect 负责：

- mailbox 初始化和销毁；
- producer raise；
- consumer 生命周期；
- payload decode hook；
- raises/delivered 统计。

RT-Thread 构建将 SoftIrq aspect 编译为空，保留直接 task-context completion；POSIX/coro 构建使用 SoftIrq mailbox。两者共享 payload 编码和验证规则，但不共享 POSIX signal API。

## 5. RT-Thread MCU 约束

所有 aspect 必须满足：

- C++17、`noexcept`、无 RTTI、无异常；
- 不使用 `new`、`malloc`、TLS、`std::function` 或运行时注册表；
- 不在 aspect 内调用 mutex、阻塞 semaphore 或线程创建；
- ISR aspect 只能调用 `record_from_isr()`、`FaultReporter` 的 ISR-safe sink 和有界 mailbox；
- 计时字段使用 `uint64_t`，写入 diag 时由 adapter 拆成低/高 32 位；
- 每个 aspect 的成员状态必须列入板级静态资源预算；
- aspect 不得持有跨 stop 生命周期的 `Event*`；
- 所有失败必须显式返回或计数，不得静默吞掉。

## 6. 分阶段实施

| 阶段 | 内容 | 验证门 | 状态 |
|---|---|---|---|
| A0 | 保留现有四层 CRTP，统一 hook 命名：`execute_job`、`produce_frame`、`consume_softirq_payload` | host/coro/RTT 编译通过 | 【已完成 bace05c】 |
| A1 | 将 `WorkerBase::run()` 的执行点抽成 `InvokePolicy` | 现有 worker 行为和 stop drain 不变 | 【已完成，行为不变验证：ctest 53/53】 |
| A2 | 接入 `MetricsAspect`，补 direct/dispatcher/worker elapsed 统计 | `pool.used()==0`、耗时累计非零 | 【已完成：per-worker execution_duration_ns】 |
| A3 | 接入 `TraceAspect`，Trace 默认关闭 | `COACT_TRACE=0/1` 符号和行为对照 | 【已完成：kEvtWorkerExec=0x0103，COACT_TRACE 门控】 |
| A4 | 接入 `FaultAspect` 和 `FaultReporter` | 水位、completion reject、FIFO overflow 边沿正确 | 【已完成：null-fn 零开销 FaultReporter 边界】 |
| A5 | 将 `IrscWorker` 和 `UsbDmaWorker` 接入对应策略链 | host/coro/RTT 双后端矩阵 | 【已完成：IrscWorker 逐帧 Trace；UsbDmaWorker 保留既有 SoftIrq 统计避免双计】 |
| A6 | 删除重复的具体 worker 辅助代码 | diff 审查、栈/静态内存预算复核 | 【已完成：预算表补 aspect 状态行】 |

每一阶段只允许一个行为变量变化。若策略链导致模板错误或栈增长超预算，回退到上一阶段，不引入运行时多态作为补偿。

## 7. 测试与验收

### 编译

- host 默认 pthread；
- `ISP_DEMO_CORO` 单 pump；
- `ISP_DEMO_USE_RTT` + `COACT_RTT_STUB`；
- 真实 RT-Thread：`RT_CPUS_NR=1`、不启用 `RT_USING_SMP`；
- `COACT_TRACE=0/1` 两种配置；
- `-fno-exceptions -fno-rtti` 和 lock-free atomic 静态门。

### 行为

- completion worker FIFO、满环拒绝和 stop drain 不变；
- IRSC 帧节拍、双目标扇出和 `frame_id` 不变；
- USB SoftIrq raises == delivered，RT-Thread 直接完成路径不依赖 POSIX signal；
- Trace/Fault 不改变 AO 状态机、事件引用计数和 DDR ownership；
- 正常路径 reject、stale、overrun、pool leak 断言保持原结果。

### 资源

- 统计每个 worker 的静态对象大小；
- 记录 Dispatcher、worker、RT-Thread main task 的最大栈水位；
- 对比策略链前后的代码尺寸和热路径耗时；
- 任何新增 aspect 状态必须更新 `common.hpp` 预算表。

## 8. 不采纳的方案

- 不引入通用运行时 AOP 容器、aspect 注册表或动态切面开关；
- 不让普通 worker 继承 `AoBase`；
- 不让 Trace/Fault aspect 直接 include diag 实现；
- 不把所有 worker 合并成一个拥有大量条件分支的万能基类；
- 不用异常机制表达硬件失败；
- 不在 MCU 路径使用 stackful coroutine、signalfd 或 POSIX 专属同步原语。

## 9. 最终判断

采用“命名能力基类 + `InvokePolicy` 静态策略链”是当前 `isp_pipeline` 最稳妥的 AOP 形态：

- CRTP 保留具体 worker 的静态身份和零虚调用开销；
- aspect 链负责计时、Trace、Fault 等横切能力；
- Completion、Periodic、SoftIrq 三种执行语义互不污染；
- RT-Thread MCU 的 ISR、静态资源和无异常约束可以在编译期保持可见。

第一步应只对 `IspIrqWorker` 做试点，验证 `InvokePolicy` 不绕过当前 `CompletionWorkerBase`，再扩展到其他 worker。
