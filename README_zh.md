**中文** | [English](README.md)

# coact

[![CI](https://github.com/DeguiLiu/coact/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/coact/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

coact（**Co**operative **Act**ive-object framework）是一个面向运行 RT-Thread 的 MCU 的 C++17 事件驱动框架，基于**主动对象（Active Object）+ 层次状态机（HSM）**，在单核上提供确定性的异步事件调度。

**RT-Thread 是首选目标平台。** Linux（host）保留为开发、测试与 SMP 参考——同一套头文件两个平台都能编，仅需切换 PAL。

## 解决的问题

裸机 RTOS 事件循环通常要求每个模块手写 mailbox + 等待逻辑。coact 把这件事抽象成可复用的流水线：事件从定容池取出，由**任务**或 **ISR** 上下文提交，经三级队列路由，由单线程派发，再回收回池——配合引用计数，同一事件可安全地扇出给多个 AO。

## 可靠承诺（framework 实际保证的）

- **热路径无堆**。事件来自定容 `EventPool`，Dispatcher 批量回收。`-fno-exceptions -fno-rtti`。
- **32 位 MCU 无锁且不依赖 libatomic**。池 free-list 是单个 32-bit 带标签索引（`[15:0]=索引 / [31:16]=ABA tag`），CAS 在 Cortex-M 上是原生指令。ISR 安全由注入的 `CriticalSection` 保证，在 RT-Thread 上映射为 `rt_hw_interrupt_disable/enable`。
- **唤醒确定性**。仅在 Dispatcher 空闲时才 signal；drain 复查封闭 missed-wakeup 窗口。`submit_from_task` 可让 producer 直接派发（S6 快路径）；`try_submit_from_isr` 永不阻塞。
- **内置背压**。熔断器（Normal → BrokenL1 → BrokenL2 → Safe → Recovering）在过载时降级而非静默丢弃；低优先级分区老化出队而非饿死。
- **编译期结构**。HSM 转移是静态表；AO 预算（`kMaxAo`、队列容量）集中在一个 `Config`。

## 目标平台

| 目标 | 默认队列后端 | 同步原语 | 说明 |
|---|---|---|---|
| **RT-Thread 5.2.x 单核**（首选） | `SingleCoreCriticalRing`（irq-mask，无原子） | `rt_hw_interrupt_disable/enable` | Cortex-M 原生 32-bit CAS，零堆 |
| Linux host（兼容） | `BoundedMpscQueue`（Vyukov） | 原生 CAS | 用于开发 / 测试 / SMP 参考 |

选平台即选 PAL：RT-Thread 用 `coact/pal_rtthread.hpp`，host 用 `coact/pal_posix.hpp`。其余框架完全一致。

## 快速开始（host）

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build          # host 测试套件
```

绑定一个 AO 并运行：

```cpp
#include "coact/runtime.hpp"
#include "coact/pal_posix.hpp"

coact::pal::Posix pal;
coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
rt.bind(&my_ao);      // my_ao : coact::Ao<Ctx, Hsm, Traits>
rt.initialize();
rt.start();
```

## 在 RT-Thread 上

包含同一组头文件，改用 `coact/pal_rtthread.hpp`，并把 `src/core/pal_rtthread.cpp` 编进 BSP（它只用内核 API：信号量、线程、`rt_hw_interrupt_disable/enable`、`rt_tick_get`）。Dispatcher 以普通 RT-Thread 线程运行；producer 调 `coordinator().submit_from_task(...)`，ISR 调 `try_submit_from_isr(...)`。运行示例见 [`examples/README.md`](examples/README.md)。

## 示例

- `examples/hsm_protocol_demo.cpp` — 层次协议状态机（父状态事件继承），完整走 pool → submit → 队列 → dispatch。
- `examples/node_manager_demo.cpp` — 一个 runtime 下四个心跳驱动的节点 AO，表序 guard。
- `examples/serial_ota/` — coact + newosp 搭的串口 OTA bridge。

每个示例的用途、构建/运行方式，以及框架分层与事件交互时序图，详见 [`examples/README.md`](examples/README.md)。

## 模块

| 组成 | 头文件 | 职责 |
|---|---|---|
| event / pool | `event.hpp` `pool.hpp` | 事件、引用计数、全局池注册表、无锁池 |
| hsm | `hsm.hpp` | HSM、父状态继承、静态转移表 |
| queue | `queue.hpp` | MPSC / 单核临界区环形队列、cache-line 隔离 |
| ao | `ao.hpp` | 主动对象、单执行权租约、注册表 |
| staging | `staging.hpp` | 三级队列、批处理、Low 老化 |
| monitor | `monitor.hpp` | 熔断器、水位、RTC 超时 |
| policy | `policy.hpp` | 限速 / 策略钩子 |
| core | `coordinator.hpp` `dispatcher.hpp` `runtime.hpp` | 提交管线、派发循环、装配 |
| pal | `pal_posix.hpp` `pal_rtthread.hpp` | 平台抽象 |

## 测试

默认 14 个 host 测试目标通过（`ctest`），池 / Dispatcher 路径 TSan 无竞争，CI（`.github/workflows/ci.yml`）跑 host + ASan/UBSan；`serial_ota_demo` 需 `-DCOACT_BUILD_SERIAL_OTA=ON`（依赖树外 newosp 头文件）。开发期间已在 RT-Thread 5.2.1 / qemu-vexpress-a9（单核）上完成启动验证。

## 许可

MIT，见 `LICENSE`。
