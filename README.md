[中文](README_zh.md) | **English**

# coact

[![CI](https://github.com/DeguiLiu/coact/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/coact/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

coact (**Co**operative **Act**ive-object framework) is a C++17 event-driven
framework for MCUs that run RT-Thread. It schedules asynchronous events through
**Active Objects (AO)** and **Hierarchical State Machines (HSM)**, giving a
deterministic, preemptive-safe dispatch model on a single core.

**RT-Thread is the primary target.** Linux (host) is kept working as a
development, test and SMP reference — the same headers build both, with a small
PAL swap.

## Why it exists

Bare-metal RTOS event loops leave the developer to hand-write mailbox + wait
logic per module. coact raises that to a reusable pipeline: an event is
allocated from a fixed pool, submitted from *task* or *ISR* context, routed
through a three-tier queue, dispatched by a single thread, and recycled back
to the pool — with reference counting so the same event can fan out to several
AOs safely.

## Design goals (what it actually guarantees)

- **No heap on the hot path.** Events come from a fixed-size `EventPool`; the
  dispatcher batch-reclaims them. `-fno-exceptions -fno-rtti`.
- **Lock-free on 32-bit MCUs without libatomic.** The pool free-list is a
  single 32-bit tagged index (`[15:0]=index, [31:16]=ABA tag`); the CAS is a
  native instruction on Cortex-M. ISR safety comes from an injected
  `CriticalSection` that maps to `rt_hw_interrupt_disable/enable` on RT-Thread.
- **Deterministic wakeups.** The dispatcher is signalled only when it is idle;
  a drain-check closes the missed-wakeup window. `submit_from_task` may run the
  AO directly (S6 fast path); `try_submit_from_isr` never blocks.
- **Back-pressure built in.** A breaker (Normal → BrokenL1 → BrokenL2 → Safe →
  Recovering) degrades under overload instead of dropping silently, and the
  low-priority partition ages out instead of starving.
- **Compile-time structure.** HSM transitions are static tables, and the AO
  budget (`kMaxAo`, queue capacities) is one `Config`.

## What runs where

| Target | Default queue backend | Synchronization | Note |
|---|---|---|---|
| **RT-Thread 5.2.x, single-core** (primary) | `SingleCoreCriticalRing` (irq-mask, no atomics) | `rt_hw_interrupt_disable/enable` | Cortex-M native 32-bit CAS, zero heap |
| Linux host (compat) | `BoundedMpscQueue` (Vyukov) | native CAS | for dev / tests / SMP reference |

Choose the PAL: `coact/pal_rtthread.hpp` for RT-Thread, `coact/pal_posix.hpp`
for host. The rest of the framework is identical.

## Quick start (host)

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build          # host test suite
```

Bind one AO and run:

```cpp
#include "coact/runtime.hpp"
#include "coact/pal_posix.hpp"

coact::pal::Posix pal;
coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
rt.bind(&my_ao);      // my_ao : coact::Ao<Ctx, Hsm, Traits>
rt.initialize();
rt.start();
```

## On RT-Thread

Include the same headers, pick `coact/pal_rtthread.hpp`, and compile
`src/core/pal_rtthread.cpp` into the BSP (it only uses the kernel API:
semaphores, threads, `rt_hw_interrupt_disable/enable`, `rt_tick_get`). The
dispatcher runs as a normal RT-Thread thread; producers call
`coordinator().submit_from_task(...)` and ISRs call `try_submit_from_isr(...)`.
See `examples/` for running AOs.

## Examples

- `examples/hsm_protocol_demo.cpp` — hierarchical protocol HSM (parent-state
  event inheritance), full pipeline: pool → submit → queue → dispatch.
- `examples/node_manager_demo.cpp` — four heartbeat-driven node AOs under one
  runtime, table-ordered guards.
- `examples/serial_ota/` — serial OTA bridge built on coact + newosp.

## Modules

| Piece | Header | Responsibility |
|---|---|---|
| event / pool | `event.hpp` `pool.hpp` | event, ref-counting, global pool registry, lock-free pool |
| hsm | `hsm.hpp` | HSM, parent-state inheritance, static transition tables |
| queue | `queue.hpp` | MPSC / single-core critical ring, cache-line isolation |
| ao | `ao.hpp` | active object, single-execution lease, registry |
| staging | `staging.hpp` | three-tier queues, batch selection, low aging |
| monitor | `monitor.hpp` | breaker, watermark, RTC timeout |
| policy | `policy.hpp` | rate-limit / policy hooks |
| core | `coordinator.hpp` `dispatcher.hpp` `runtime.hpp` | submit pipeline, dispatch loop, assembly |
| pal | `pal_posix.hpp` `pal_rtthread.hpp` | platform abstraction |

## Testing

14 host test targets pass by default (`ctest`), TSan-clean on the
pool/dispatcher paths, plus `serial_ota_demo` when built with
`-DCOACT_BUILD_SERIAL_OTA=ON` (needs newosp headers out of tree). CI runs host
+ ASan/UBSan via `.github/workflows/ci.yml`. The framework was brought up on
RT-Thread 5.2.1 / qemu-vexpress-a9 (single-core) during development.

## License

MIT — see `LICENSE`.
