# coact 热路径剖析：无 perf 环境下的 CPU 采样与无锁池的缓存行争用

> 只回答一件事：在不能装 perf、没有 root 的 Linux 上，如何把 coact 派发热路径的 CPU 占用测出来、测准确，并解释两个看似矛盾的结果——单核下最贵的是平台同步而不是框架逻辑，多核下无锁池反而让 staged 吞吐倒退。全文结论都可从 `src/core/bench_hotpath.cpp` 与 `include/coact/{pool,dispatcher}.hpp` 回溯。

## 一、先想清楚要测什么

微基准最怕"测了业务，却说成框架"。coact 的基准把关心点收得非常窄：

- 状态机是零业务语义的自环：`kTrans` 里只有一条 Internal 转移，guard 恒真，action 只做一次 `fetch_add(1)` 计数。整条链 `submit → staging → Dispatcher → Ao::dispatch → HSM → event_gc` 都是框架本身，业务时间趋近于零。
- 两种派发机制被拆成互斥的 mode：`staged`（2 个非 direct AO + 2 个 producer）与 `direct`（1 个 direct-eligible AO + 1 个 producer）。不混测不是风格洁癖，而是 direct 与 queued 派发共享执行租约，混在一个 AO 上会触发既有的 direct/staged lease 竞争，把采样结果污染掉。

于是基准回答的问题收敛成一句：**框架把一事件从池里取出、排队、派发、转移、再还回池，这段自身要花多少 CPU。**

## 二、穷人采样器的正确姿势

没有 perf（通常意味着没有 root 去碰 `perf_event_open`），用手写采样器：`SIGPROF` 间隔定时器加 `backtrace()`。但"能采到栈"和"采得准"之间隔着几个不显而易见的坑。

**第一，选 `ITIMER_PROF` 而不是 `ITIMER_REAL`。**
`ITIMER_PROF` 只在进程真正消耗 CPU（用户态 + 内核态时间）时才给定时器计费；进程阻塞在 condvar 或 sleep 上时定时器不前进。所以这个采样器本质是"CPU 占用采样"，不是墙钟采样。火焰图里只出现真正烧 CPU 的帧，天然排除了等待时间——这恰好就是我们要的量。

**第二，handler 只写 ring，不做任何重操作。**
信号处理器里不能碰 `malloc`、`printf`、`dladdr`、`__cxa_demangle`，它们都不是 async-signal-safe。因此 `sample_handler` 只做两件事：`fetch_add` 取槽位，`backtrace()` 把最多 48 帧写进预分配的 16384 槽环形缓冲；符号化整体推迟到进程退出后，再用 `dladdr` 加 `__cxa_demangle` 完成。`backtrace` 本身在 glibc 的 async-signal-safety 清单上也是灰色地带，但配合 `SA_RESTART`、预分配缓冲和受控采样频率，在剖析工具里是可接受的——这才是"穷人采样器"真正的工程权衡点，而不是"能跑就行"。

**第三，把采样器自己从样本里剔除。**
`skip_frame` 过滤 `sample_handler`、`backtrace`、`__kernel_rt_sigreturn`、`restore_rt`。不做这一步，每个样本顶端都会是采样器自身的帧，等于把相当一部分样本烧在"采样这件仪器"上，结论立刻失真。

**第四，折叠 root→leaf 再画火焰图。**
退出时把每个栈按 root→leaf 用分号拼成一条路径，同路径累加计数，交给 `tools/flamegraph_svg.py` 渲染成 SVG。条形宽度等于该帧在所有样本中的占比——这是"CPU 燃烧比例"的统计估计，不是逐周期耗时。

16384 槽 @ 1 kHz 约等于 16 秒的连续覆盖，对一次 5 秒的基准足够。`bench_hotpath` 这个 target 本身已经强制 `-O2 -g -fno-omit-frame-pointer` 并链接 `-rdynamic`：`backtrace` 要拿到完整帧指针和被保留的动态符号，否则采样结果全是 `[unknown]`。

## 三、把基准按回部署环境

裸跑 `bench_hotpath` 默认落在 host 多核上，测出来的是 SMP 竞争，跟单核 Cortex-M 目标南辕北辙。两个参数把它按回去：

| 参数 | 作用 | 对应目标语义 |
|---|---|---|
| `--cores 1` | `sched_setaffinity` 把全进程钉到 CPU 0，线程继承亲和性 | 单核上 producer 与 Dispatcher 互相抢占 |
| `--tick-hz 100` | 单调时钟按 10 ms 量化 | RT-Thread 100 Hz tick 的时间粒度 |

不指定 `--cores` 时测的是线程并行的多核吞吐；只有钉到单核，condvar 唤醒、上下文切换这些真实部署成本才会浮出来。

## 四、发现一：单核下最贵的是平台，不是框架

单核 100 Hz staged（-O2）的火焰图给出两个大头：condvar 唤醒+等待约 31%，`clock_gettime` 约 14%。二者合计接近一半，而队列、HSM 线性查表、池分配这些"框架逻辑"并不在前排。

condvar 那部分对应 Dispatcher 与 producer 的唤醒协作，优化落点是"只在 Dispatcher 空闲时才 signal"，避免每个事件都踢一脚睡着的 Dispatcher，实测 signal 调用下降约 28%。

`clock_gettime` 那 14% 更值得玩味——它是刻意付的。Dispatcher 在**每次 dequeue 都刷新 `now_ns`**，而不是一个 batch 只取一次。原因是 Low 分区的老化判定要用当前时间：如果复用 batch 开始时的陈旧时钟，batch 中途到达的 Low 事件会被误判为"没老化"或提前强制服务。所以它不是没优化好的泄漏，而是一处"正确性优先于时钟开销"的显式取舍。想省掉这 14%，首先要改 Low 老化的语义，而不是改采样器。

拎出来的教训比数字重要：**先区分平台开销和逻辑开销**。单核下框架逻辑几乎免费，代价集中在 OS 同步原语和时钟读取上；这些在 C/C++ 里都一样，与语言选型无关。

## 五、发现二：多核下无锁池改了，却倒退

多核环境下火焰图顶格的是 `EventPool` 的 `std::mutex`，约占 47%。于是把池改成无锁：Treiber 栈加 32 位 packed head（低 16 位索引 / 高 16 位 ABA tag），用 `compare_exchange_weak` 完成弹出与压回。

结果却分裂：

| 模式 | 结果 | 吞吐 |
|---|---|---|
| direct | +13% | 15.4M → 17.4M ev/s |
| staged | -12~15% | 0.90~1.22M vs 1.38M ev/s |

direct 变快符合直觉：无竞争时一次 CAS 分配比一次 mutex 更快。staged 倒退才是这次剖析最有价值的部分。

## 六、为什么会倒退：一个共享 head 的缓存行 ping-pong

staged 模式有多个 producer 同时 `alloc`（弹出 free-list 头），唯一的 Dispatcher 在 `reclaim`（压回 free-list 头）。两边操作的原子变量是**同一个 `free_head`**。

无锁反而更慢的根因，躲在"失败者做什么"里。`std::mutex` 的失败者在短暂自适应自旋后通常进入内核 futex 等待（park），把 CPU 让出去、不再持续撞那行 cache line；而 Treiber CAS 的默认失败路径是立即重试——每次失败都是一次独占态的读写修改，对着同一条 `free_head` 行反复 invalidate。多核 staged 下，无锁结构把"阻塞"换成了"自旋"，自旋的目标又是单一共享 cache line，于是量出来的反而比会 park 的 mutex 更贵。

两个设计杠杆随后被放进去约束这个问题：

- **有上限的指数退避 `pool_backoff`**：CAS 失败后不在失败点立刻重做（那会把独占态流量放大），而是退避地做若干次 relaxed load。relaxed load 是共享态、不引发写竞争，等 winner 的写落地、line 重新安静后再重试。退避封顶在 `2^3=8` 次，避免在已经很烫的行上无脑自旋。
- **批量回收 `BatchedReclaimer`**：Dispatcher 每批至多处理 `kBatchSizeMax=8` 个事件，本应每事件一次 `free_head` CAS 的回收被折叠成"每池一条链、一次 splice"（`kReclaimBatchCap=16`）。这把单消费者 reclaim 侧的绝大部分 CAS 消掉。

但这两个杠杆主要缓解 reclaim 侧，alloc 侧仍是多个 producer 打同一个 `free_head`。批量回收能降"压回"的次数，降不掉"弹出"的竞争，所以 staged 的倒退没有因批量回收而彻底追回——要让多 producer 的 alloc 也走向多头（sharded）或批量分配，才可能扳回来。

## 七、结论要落在部署场景，而不是 benchmark 环境

这里有个最容易忽略的反转：**真正的部署目标是单核 RT-Thread，而这次无锁化是在多核 host 上为消除互斥竞争和 TSan 验证做的。** 单核目标上池的同步后端根本不是 tagged-CAS，而是 `RttSingleCoreProfile`——在 irq 屏蔽临界区里做 plain index/head，没有 CAS、没有 ABA tag、没有退避。单核上池锁从来不是热点，无锁化对部署场景是中性甚至多余的。

所以"锁好还是无锁好"没有普世答案，取决于竞争形态：

| 竞争形态 | 结论 |
|---|---|
| 单核 / 单一 irq-屏蔽临界区 | CAS 退化为普通临界区，无收益也无需 CAS |
| 单 producer + 单 consumer | 无锁加批量回收即可，无 ping-pong |
| 多 producer 打单 head | 单共享 head 是瓶颈；需多头/批量分配，否则无锁也倒退 |

benchmark 环境会奖励"多核下最漂亮的结构"，部署环境只奖励"目标上真实的最短路径"。一次有效的热路径剖析，负责先把这两者分开；多核下无锁池在 staged 模式倾泻的 cache-line 迁移，其实在提醒我们：任何单一共享头的结构，都不存在"越无锁越快"的通行证。

## 八、方法论边界

- 采样是统计估计：条宽是样本占比，误差随样本数下降，但不是逐周期测量。
- Debug 构建会虚高框架占比，结论必须以 -O2 为准（`bench_hotpath` target 已强制）。
- `backtrace` 依赖 `-fno-omit-frame-pointer` 与 `-rdynamic`，并受 -O2 内联影响。
- 火焰图回答"CPU 烧在哪里"，不回答"为什么"。"为什么"要回代码找：例如 `clock_gettime` 那 14% 来自 per-dequeue 的 Low 老化刷新，写在 `dispatcher.hpp` 的注释里，而不在火焰图里。

## 复现

```sh
cmake -B build_bench -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_bench --target bench_hotpath -j
# 单核 100 Hz staged：
./build_bench/src/core/bench_hotpath --mode staged --cores 1 --tick-hz 100 \
    --hz 1000 --seconds 5 --sample staged.folded
python3 tools/flamegraph_svg.py staged.folded staged.svg "coact staged 1c/100Hz"
```

---

*事实依据：`src/core/bench_hotpath.cpp`（ITIMER_PROF 采样器、双 mode、亲和性、时钟量化、`skip_frame`）、`tools/flamegraph_svg.py`、`include/coact/pool.hpp`（`RttSingleCoreProfile` / `HostSmpProfile` 双后端、`pool_backoff`、`BatchedReclaimer` / `ReclaimBatcher`、`pool_reclaim_chain`）、`include/coact/dispatcher.hpp`（per-dequeue `now_ns`、idle 唤醒）、`src/core/CMakeLists.txt`（`bench_hotpath` 的 `-O2 -g -fno-omit-frame-pointer -rdynamic`）。*
