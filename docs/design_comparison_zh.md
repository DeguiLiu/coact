# coact vs newosp 核心代码设计对比

> 仅对比核心层代码设计，不涉及文档与示例。
> 数据来源：coact `include/coact/*.hpp`，newosp `include/osp/*.hpp`，均经代码核实。

---

## 1. 消息/事件模型

| 维度 | coact | newosp |
|---|---|---|
| 消息表示 | `Event{signal:u16, pool_id:u8, ref_ctr:u8}` 4 字节定长头 | `MessageEnvelope<PayloadVariant>`：header + `std::variant` payload，运行期大小 |
| 所有权语义 | QP 式引用计数：`event_ref_inc` / `event_gc`；最后一次 `gc` 把块还给池 | Bus 路径 move 语义（无 ref_ctr）；DataDispatcher 路径有 `atomic<uint32_t> refcount` CAS |
| 零拷贝 | 全程指针传递，pool block 原地构造；Staging 存 `Event*`，Dispatcher 拿指针派发 | Bus: payload 移动进 ring buffer slot，消费者持 `const EnvelopeType&` 只读引用 |
| 静态/动态事件 | `pool_id==0` 标记静态事件，`event_gc` 跳过，框架不触碰 | 无等价概念，所有消息等同处理 |

**结论**：coact 参照 QP/QF 的引用计数+池回收是更严格的**所有权契约**，适合多消费者多播而不泄漏；newosp Bus 的 move 语义更简洁，但不支持多播共享（move 后源为空）。DataDispatcher 路径两者都有引用计数，设计相近。

---

## 2. 内存池

| 维度 | coact `EventPool` | newosp `FixedPool` | newosp `DataDispatcher store` |
|---|---|---|---|
| free-list 类型 | 索引式，块首 4 字节存 next index | 索引式，块首 `uint32_t` 存 next | 索引式，`DataBlock::next_free` |
| 并发策略 | 32-bit tagged-CAS（LDREX/STREX 原生）+ 注入 `CriticalSection`（irq mask） | `osp::Mutex`（`std::mutex` 封装），每次 Alloc/Free 加锁 | 64-bit tagged-CAS，`atomic<uint64_t> free_head_`，高 32 bit ABA tag |
| ABA tag | 16 bit tag（`[31:16]`），32-bit 原子，无 libatomic | 无 | 32 bit tag（`[63:32]`），64-bit 原子 |
| 目标平台 | 32-bit Cortex-M 原生无锁 | Linux 只，std::mutex 不可用于裸机 ISR | Linux SMP 优化 |
| 批量回收 | `ReclaimBatcher`：Dispatcher 尾部 flush，多事件一次 CAS splice，热路径实测 ~42% 提升（注：该数字来自修复前基线；SMP 下为消除块 `next` 字段竞争引入 spinlock CS 后需复测） | 无 | 无 |

**结论**：newosp `FixedPool` 用互斥锁——无法在 RT-Thread ISR 上下文调用；coact `EventPool` 32-bit CAS + irq mask 是 Cortex-M 单核的正确选择。newosp `DataDispatcher` 的 64-bit ABA 更适合 64-bit SMP Linux。`ReclaimBatcher` 是 coact 独有的批量回收优化。

---

## 3. 队列

| 维度 | coact `BoundedMpscQueue` | coact `SingleCoreCriticalRing` | newosp `AsyncBus` MPSC | newosp `SpscRingbuffer` |
|---|---|---|---|---|
| 并发模式 | MPSC | 单核，irq mask | MPSC | SPSC |
| 算法 | Vyukov per-slot sequence，head CAS | irq 临界区内 head/tail | Vyukov per-slot sequence | wrap-around，power-of-2 |
| 容量 | 编译期模板参数 | 编译期 | 宏 `OSP_BUS_QUEUE_DEPTH`，编译期 | 模板参数，编译期 |
| 满处理 | `try_push` 返回 false | `try_push` 返回 false | 按优先级水位（60%/80%/99%）丢弃 | `Push` 返回 false |
| 优先级 | 无（由 Staging 三分区实现） | 无 | 单队列内按 `MessagePriority` 水位 | 无 |
| cache-line 隔离 | head/tail 各 64 字节 padding | n/a | bus.hpp 未见显式 padding | head/tail `PaddedIndex`，各 cache-line |

**结论**：coact 把优先级管理提升到 **Staging 三分区**（High/Normal/Low 独立队列），比 newosp 在单队列内按水位丢弃更精细，Low aging 防饥饿；newosp `SpscRingbuffer` 的 `FakeTSO` 模式可按平台退化全 relaxed，适合单核裸机 SPSC。

---

## 4. HSM

| 维度 | coact `Hsm` | newosp `StateMachine` |
|---|---|---|
| 状态注册 | 编译期 `StateDef[]` / `TransitionDef[]` 静态表，构造传指针 | 运行期 `AddState()` 填 `std::array<StateConfig, MaxStates>` |
| 状态标识 | `int8_t` 索引（-1 = 无父/未初始化），实际可用非负索引 0..126；有 `current_state_name()` 取调试名 | `int32_t` 索引，调试有 `const char* name` |
| 转移查找 | 线性扫描 `(source, signal)`，自叶向父 bubble-up，`max_depth` 限跳数 | LCA 深度归一化，父态 bubble-up，`OSP_HSM_MAX_DEPTH`（默认 32）栈上 path[] |
| 层级继承 | 父态哨兵 `-1`，自叶向上找到第一个匹配 | 返回 `kUnhandled` 向父传递，入口自顶向下 |
| 回调 | `entry/exit` + `guard/action` 函数指针，可 null | `HandlerFn/EntryFn/ExitFn/GuardFn` 四个裸函数指针 |
| 内存 | 零分配，只存静态表地址 | `std::array` 内联，无堆分配 |
| 最大状态数 | `int8_t` 上限 127 | 编译期模板参数，默认 16 |

**结论**：coact 编译期表更适合 MISRA + 静态分析（状态机结构在编译期确定，可用工具验证），但状态数受限于 `int8_t`（非负索引 0..126，`-1` 作哨兵）；且 coact `Hsm` 同样支持 `current_state_name()` 取调试名（由 AO 提供名字表），调试能力与 newosp 相当。newosp 运行期 `AddState` 更灵活并自带调试名，但状态表在运行时才完成，不适合要求编译期可证状态机结构的安全认证场景。

---

## 5. 执行模型（主动对象与 RTC）

| 维度 | coact | newosp |
|---|---|---|
| 主动对象原语 | `Ao<Context, Hsm, Traits>` 明确类型，`ExecutionLease` CAS 保证单执行 | 无独立"主动对象"类型；`Application<Impl>` 最接近，每实例独立 SPSC 队列 + RTC 语义 |
| 互斥保证 | `ExecutionLease::try_acquire` 原子 CAS（`Idle→RunningDirect/RunningDispatcher`），失败非阻塞 fallback staging | `node.hpp` 注释"Only ONE thread should call SpinOnce at a time"，无硬性原语保证 |
| 直接调度 | `dispatch_direct`：producer 线程自己跑 HSM，bypass Dispatcher（M1 路径） | 无等价机制，消息必须进 bus ring |
| Dispatcher | 单线程批处理，`begin_batch`/`mark_active`/`mark_idle`，missed-wakeup-safe | executor 驱动 `ProcessBatch` 循环，无明确 batch 语义 |

---

## 6. 过载保护与可观测

| 维度 | coact | newosp |
|---|---|---|
| 熔断 | `Breaker<Config>`：五态（Normal/BrokenL1/BrokenL2/Safe/Recovering）降级状态机，cooldown 计数，水位触发，L1→L2→Safe 降级链，Recovering 回 Normal | 无等价状态机；只有 bus 三级水位准入控制 |
| 水位 | Staging `watermark()` 按分区（High/Normal/Low）返回 0-100% | Bus `GetBackpressureLevel()`（kNormal/kWarning/kCritical/kFull）单队列级 |
| 遥测 | `Monitor<Config>`：`GlobalCounters`/`AoCounters` 全部 relaxed atomic（本轮 MISRA 修正后） | `BusStatistics` 4 个 cache-line 对齐 `atomic<uint64_t>`；DataDispatcher `BackpressureFn` 回调 |
| 低优先级饥饿 | Low aging：`low_head_arrival_ns_` 超时强制出队（kLowMaxWaitMs） | 无 aging；单队列水位丢弃，低优先级在高水位时最先丢 |

---

## 7. 平台抽象

| 维度 | coact | newosp |
|---|---|---|
| 方式 | `PalT` 模板参数（concept 检查），`pal.hpp` 定义抽象/辅助（`CriticalSection`/`SpinCriticalSection`），`pal_rtthread.hpp` / `pal_posix.hpp` 两个具体实现 | 宏检测（`__has_include <rtthread.h>`），`SteadyNowUs()`/`CpuRelax()` 条件编译 |
| irq mask | `CriticalSection{ctx, save, restore}` 函数指针注入，RT-Thread 映射 `rt_hw_interrupt_disable/enable` | 无 irq mask 抽象；只有 `SpinLock`/`Mutex`，不可在 ISR 中持有 |
| ISR 安全 | `signal_dispatcher_from_isr()` + `try_submit_from_isr()` 明确 ISR 路径；`SingleCoreCriticalRing` 以 irq mask 保护 | 无 ISR 安全队列；`rt_sem_release` 只在 RTT PAL 封装 |
| RT-Thread | 一等公民：`pal_rtthread.hpp` 完整实现（rt_sem/rt_thread/irq_save/tick_get），test stub | 支持，但 irq/ISR 路径无专用抽象 |

---

## 8. 编译期配置

| 维度 | coact | newosp |
|---|---|---|
| 机制 | 单一 `Config` 结构（`DefaultConfig`）贯穿 `AoRegistry<Config>`、`Monitor<Config>`、`Breaker<Config>`、`Staging<Config,Backend>`——统一一处改，全链路生效 | opt.hpp 集中 30+ `OSP_*` 宏（`#ifndef` 覆盖式），无统一 Config 对象 |
| 容量传递 | 编译期模板参数从 Config 传入各组件，类型系统保证一致性 | 宏给模板默认值，各组件模板参数独立，可能不一致 |

---

## 9. 并发内存序纪律

两库均全程显式 memory_order，无 seq_cst 兜底——这是正确的高性能并发实践。

coact 额外：
- `dispatcher_active_` release/acquire + Dispatcher 退出前 `any_buffered()` 再检查，文档化 missed-wakeup-safe 证明
- ReclaimBatcher：批内链写与 flush 的 head CAS 均在注入的 CriticalSection（SMP=spinlock，单核=irq mask）内完成，CAS 用 relaxed，串行性由 CS 提供，保证块 `next` 字段写入与并发 alloc 读互不交错

newosp 额外：
- SPSC `FakeTSO` 模式：ARM/x86 TSO 架构可退化全 relaxed，减少 barrier 开销
- `SharedSpinLock` 支持读回调内重入（`thread_local` 持锁深度），防止 Subscribe 回调内再 Subscribe 死锁

---

## 10. 安全约束对比

| 维度 | coact | newosp 核心层 |
|---|---|---|
| 热路径无堆 | 全框架，包括 Staging/Dispatcher/Ao/Pool | `spsc_ringbuffer`/`data_dispatcher`/`hsm`/`node`/`static_node` 无堆；`mem_pool` 有 mutex，`post.hpp` OspSendAndWait 有一次 `new` |
| 无异常/RTTI | 全局 `-fno-exceptions -fno-rtti` | 核心热路径头无 throw/catch/dynamic_cast；inicpp/config/semaphore 有异常 |
| MISRA 对齐 | 全部 switch 有 default，无 goto，固定宽度整型，本轮修正整数窄化与未初始化成员 | `serial_transport.hpp` 有 deviation 注释（Rule 5-2-4），核心层固定宽度整型，未全面 MISRA 审计 |
| ISR 可用 | 池/队列在 RT-Thread 下经 irq mask 保护，ISR 可直接 submit | 无 ISR 专用路径 |

---

## 总结：场景选择矩阵

| 场景 | 选 coact | 选 newosp 核心层 |
|---|---|---|
| RT-Thread 单核 Cortex-M，ISR 安全，MISRA 审计 | ✓ | ✗（mutex 不可 ISR，无 irq mask） |
| Linux SMP 多核，高吞吐 pipeline | △（MPSC 可用，但无网络/日志生态） | ✓（64-bit ABA，FakeTSO SPSC） |
| 严格编译期状态机结构（安全认证） | ✓（静态表，编译期可分析） | △（运行期 AddState，结构运行时才定） |
| 快速开发，需要调试状态名 | ✓（`Hsm::current_state_name()`，AO 提供名字表） | ✓（AddState 带 name，直接打印） |
| 多播共享事件（多 AO 同时持有） | ✓（引用计数，最后一个 gc 回池） | △（Bus move 语义不支持多播共享） |
| 统一 Config 贯穿全组件 | ✓ | △（各组件独立宏，需手动保持一致） |
