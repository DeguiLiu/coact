# coact 实现契约（Implementation Contract）v0.1

本文档是所有编码 subagent 的接口契约。**先读本文，再读 `design_coact_zh.md` 对应章节，最后动手。**

## 0. 纪律（最高优先级）

- 项目许可证：**MIT**。新文件必须有 `// SPDX-License-Identifier: MIT` 头注释。
- 语言：**C++17**（if constexpr 可用）。构建强制 `-fno-exceptions -fno-rtti`，代码不得依赖异常与 RTTI。
- 代码来源：
  - `newosp`（MIT，`/home/dgliu/newosp`）白名单组件**可复用/移植**，必须保留原 MIT 版权文本并标注来源文件。
  - `QP/C++`（`/home/dgliu/qpcpp`）、`qpc-rtthread`（`/home/dgliu/qpc-rtthread`）：**允许复用其代码、宏配置体系、全局注册表、引用计数事件模型**（用户决策 2026-08-08）。项目整体仍以 MIT License 发布；复用实现时按 coact API 适配，并在文件头注明来源仓库与文件，供合规回溯。
  - **接口契约优先**：本文档 4.x 的签名是已冻结架构（QP 式引用计数事件 + 受控全局池/AO 注册表，用户 2026-08-08 决策定稿）；如实现过程中发现需要偏离（例如改回唯一所有权、改变 ref_ctr/event_gc 语义、引入新的全局单例），属于架构变更，必须先报告协调者裁决，不得擅自推翻已冻结签名。
- 编码风格：Allman 大括号、4 空格缩进、120 列上限、无 `goto`、无动态分配（核心运行时）、错误用返回值/`Expected`、禁止全局可变单例（`constexpr` 表除外）。
- 头文件 `#pragma once`，统一放 `include/coact/`。不要修改其他模块的头文件——如有跨模块接口问题，在报告中指出，由协调者裁决。
- 允许 QP 式**受控全局注册表**（事件池注册表、AO 注册表）作为唯一允许的单例形式；其余运行状态仍显式实例化。

## 1. 仓库结构（已就绪）

```
/home/dgliu/coact/
├── CMakeLists.txt            # C++17 strict，add_subdirectory 各模块
├── LICENSE                   # MIT
├── include/coact/            # 公共头文件（config/expected/assert/pal 已由骨架提供）
│   ├── assert.hpp            # COACT_ASSERT + coact::fatal_assert
│   ├── config.hpp            # LogicalPrio/PriorityClass/ExecutionContext/EventQos/
│   │                         #   TargetId/SubmitDisposition/SubmitResult/错误枚举/DefaultConfig
│   ├── expected.hpp          # Expected<T,E>（move-only）+ Expected<void,E>
│   └── pal.hpp               # pal::CriticalToken / pal::ThreadEntry + PAL 契约注释
├── test/
│   ├── test_harness.hpp      # COACT_TEST/CHECK/REQUIRE/COACT_TEST_MAIN
│   └── CMakeLists.txt        # coact_add_test(<target> <src>...) 辅助函数
└── src/<module>/             # 每个 subagent 拥有一个模块目录
    ├── CMakeLists.txt        # 用 coact_add_test 声明测试可执行文件
    ├── ...（本模块头文件，放 include/coact/）
    └── test_<module>.cpp
```

## 2. 构建与测试（每个 subagent 独立执行）

使用**独立 build 目录**，避免并行冲突：

```sh
cd /home/dgliu/coact
cmake -B build_<mod> -S .
cmake --build build_<mod> --target test_<mod>
ctest --test-dir build_<mod> -R test_<mod> --output-on-failure
```

- 未写 CMakeLists 的模块目录目前是空占位（`add_subdirectory` 对空 CMakeLists 无害）。
- 你的模块 `src/<mod>/CMakeLists.txt` 写完后，顶层 configure 仍会对其他空模块正常通过。
- 测试用 `#include "test/test_harness.hpp"`，文件末尾 `COACT_TEST_MAIN()`。
- 若测试需要 `pthread`，在模块 CMakeLists 加 `target_link_libraries(test_<mod> PRIVATE pthread)`。

## 3. 模块、文件所有权与依赖

| 模块 | 目录 | 头文件（include/coact/） | 依赖骨架 | 依赖其他模块 |
|---|---|---|---|---|
| event | src/event | event.hpp, pool.hpp | expected, config, assert | 无 |
| hsm | src/hsm | hsm.hpp | config | event 的 `Event` 类型 |
| queue | src/queue | queue.hpp | config | 无 |
| monitor | src/monitor | monitor.hpp | config, expected | 无 |
| policy | src/policy | policy.hpp | config, assert | event 的 `Event` |
| ao | src/ao | ao.hpp | config | hsm 的 `Hsm`、event 的 `Event` |
| staging | src/staging | staging.hpp | config | queue 的队列、event 的 `Event` |
| core（集成） | src/core | coordinator.hpp, runtime.hpp, dispatcher.hpp | 全部 | 全部 |

依赖规则：只 `#include` 自己模块与所列依赖模块的头文件。禁止 include 未列出的模块。函数指针类型**不带** `noexcept` 限定（C++17 下 noexcept 不是函数指针类型的一部分）。

## 4. 模块接口契约（签名以设计文档为权威，此处补缺省）

### 4.1 event（src/event）— 见设计 §6（已切换为 QP 式引用计数 + 全局池注册表）

```cpp
struct Event {
    uint16_t signal;
    uint8_t pool_id;   // 0 = 非池（静态）事件；否则为全局池注册表索引
    uint8_t ref_ctr;   // 引用计数：alloc 时 1，额外所有权 inc，gc dec，归 0 回池
};

// 全局池注册表（受控单例，QP QF_pool_ 式）。初始化后按 pool_id 定位。
struct PoolRecord {
    void* free_list;         // 空闲块链表头
    uint16_t block_size;     // 事件块对齐大小
    uint16_t capacity;
    uint16_t used;
    uint16_t high_watermark;
    void (*reclaim)(Event* e) noexcept;   // 归还块
};
PoolRecord* pool_record(uint8_t pool_id) noexcept;    // 未知池返回 nullptr
uint8_t register_pool(PoolRecord* rec) noexcept;      // 分配 pool_id，注册表满返回 0

// 事件池：定容、块内空闲链表（复用 newosp mem_pool.hpp 思路）、alloc 后 ref_ctr==1
template <uint16_t BlockSize, uint16_t Capacity,
          class Profile = HostSmpProfile,
          size_t BlockAlign = alignof(std::max_align_t)>
class EventPool {
public:
    bool init(void* storage, size_t bytes,
              CriticalSection cs = detail::noop_cs()) noexcept;
    Event* alloc(uint16_t signal) noexcept;           // 满返回 nullptr；ref_ctr=1
    template <typename Layout, typename Payload, size_t PayloadAlign>
    Layout* alloc_typed(uint16_t signal) noexcept;
    uint16_t used() const noexcept;
    uint16_t high_watermark() const noexcept;
};

// 引用计数管理（QP QF 语义）：
//   submit 接管 allocation reference；额外投递/自留所有权前 event_ref_inc(e)
//   event_gc 递减 ref_ctr；归 0 且 pool_id!=0 → 经 pool_record(pool_id) 回原池
void event_ref_inc(Event* e) noexcept;
void event_gc(Event* e) noexcept;
```

`EventPool::init()` 成功初始化外部存储并完成池注册时返回 `true`；重复初始化、空存储、无效临界区、可用空间不足一个对齐块或注册表已满时返回 `false`，失败实例保持不可分配。`BlockAlign` 必须非零、为 2 的幂且不小于 `alignof(Event)`；对齐后的块步长必须能由 `uint16_t` 表示。

`alloc_typed()` 先取得原始块，再以 placement new 值初始化完整 `Layout`，写入 `Event` 头，最后在 `Layout::payload` 区域以 placement new 启动 `Payload` 生命周期。`Layout` 与 `Payload` 必须可无异常默认构造、可平凡复制且可平凡析构；`Layout` 必须是标准布局且首成员为精确的 `Event`。编译期同时校验 `sizeof(Layout) <= BlockSize`、`sizeof(Payload) <= sizeof(Layout::payload)`、`alignof(Layout) <= BlockAlign` 以及 payload 声明对齐、实际偏移和类型对齐一致。池不提供析构回调，因此回收时不执行应用类型析构函数。

生命周期规则（QP QF 语义，设计 §6.4 改为 refCtr 生命周期）：
- `alloc` 返回 `ref_ctr==1` 的池事件；生产者持有并配置 payload/signal；
- 首次 submit 转移 allocation reference，不额外 inc；多播或自留所有权时，每增加一个 owner 先 `event_ref_inc(e)`；
- 每个消费者在 `dispatch` 完成后 `event_gc(e)`（dec）；最后一个消费者把 `ref_ctr` 减到 0 时经 `pool_id` 回原池；
- 静态事件（`pool_id==0`）`event_gc` 无操作；
- 生产者投递后必须放弃访问，除非先 `event_ref_inc` 自留引用；
- 同一事件只能回创建它的原池；池销毁前所有 outstanding 必须归 0。

测试：refCtr 多播（同事件 2 次 inc + 2 次 gc 归 0 回收）、静态事件不回收、满池返回 nullptr、allocator hook 零堆、跨池 pool_id 路由正确、同一池事件 gc 后复用块。

### 4.2 hsm（src/hsm）— 见设计 §7

```cpp
enum class TransitionKind : uint8_t { External, Internal, Self };

template <typename Context>
struct StateDef {
    int8_t parent;                 // 0 表示根
    void (*entry)(Context&);
    void (*exit)(Context&);
    const char* name;
    int8_t initial_child = -1;     // 复合态的直接初始子态
};

template <typename Context>
struct TransitionDef {
    int8_t source;                 // 0 表示 root；不允许 wildcard
    uint16_t signal;
    int8_t target;
    TransitionKind kind;
    bool (*guard)(const Context&, const Event&);
    void (*action)(Context&, const Event&);
};

template <typename Context>
class Hsm {
public:
    Hsm(const StateDef<Context>* states, uint16_t num_states,
        const TransitionDef<Context>* transitions, uint16_t num_transitions,
        int8_t initial_state, uint8_t max_depth) noexcept;
    void init(Context& ctx, const Event& evt) noexcept;   // 进入 initial_state
    bool dispatch(Context& ctx, const Event& evt) noexcept;  // handled?
    int8_t current_state() const noexcept;
};
```

派发语义（设计 §7.3）：从当前叶按 `(state, signal)` 查转换，未命中沿 parent 上溯（最多 max_depth 次）；guard 失败继续同 source/signal 的下一条；internal 只 action；self 从实际叶退出到 source（含 source）、action、重进 source 并沿 `initial_child` 下降；external 按声明 source/target 的外部边界退出、action、进入 target，再沿 `initial_child` 下降。`TransitionKind` 不提供 Local：当 external 的 target 是 source 或当前叶的复合祖先时，仍须退出并重入该边界复合态，再进入其 `initial_child`；不得退化为不退出复合态的 local 转换。运行期父链与 LCA，不依赖 constexpr。函数指针不写 noexcept。

### 4.3 queue（src/queue）— 见设计 §10.2

两个编译期后端，都是**多生产者单消费者**，模板参数 `T` 任意定长类型：

```cpp
template <typename T, uint16_t Capacity>
class BoundedMpscQueue {           // POSIX：fixed-cell ready-set，生产者 release/消费者 acquire
public:
    bool try_push(const T& v) noexcept;
    bool try_push(T&& v) noexcept;
    bool try_pop(T& out) noexcept;
    bool front(T& out) const noexcept;
    bool has_ready() const noexcept;
    uint16_t size() const noexcept;
    static constexpr uint16_t capacity() noexcept;
};

template <typename T, uint16_t Capacity>
class SingleCoreCriticalRing {     // 单核：临界区内仅改索引与槽状态
public:
    // 临界区通过构造注入（函数指针或回调），宿主测试可注入 no-op
    explicit SingleCoreCriticalRing(CriticalSection cs) noexcept;
    bool try_push(T&& v) noexcept;
    bool try_pop(T& out) noexcept;
    bool front(T& out) const noexcept;
    bool has_ready() const noexcept;
    uint16_t size() const noexcept;
};
```

- `CriticalSection` 由本模块定义：`{Token (*save)(); void (*restore)(Token);}`，token 为 `uintptr_t`。单核后端 push/pop 全程包在 save/restore 内；`T` 的移动/析构也位于临界区，因此 MCU payload 必须是有界、`noexcept` 的轻量类型。
- `BoundedMpscQueue` 使用 fixed-cell ready-set：producer 独占 Writing cell，完成构造后 release 为 Ready；consumer 在固定 cell 集中选当前可见 publication ticket 最小项。Writing 只占一格，不阻塞其他 Ready 项；单线程 push 保持 FIFO，但并发 push 不承诺 per-producer FIFO（后发布且已完成的项可绕过仍在构造的项）。该后端要求无锁 64-bit 原子；push 满返回 false，pop 无 Ready 项返回 false；测试必须覆盖多生产者并发不丢失、不重复、不越界（可断言序号审计）。队列析构要求生产者和消费者已停止，并在析构时销毁仍存活的非平凡 payload。
- `front()` 只复制当前 Ready 队首，不消费元素；它与 `try_pop()` 同属单消费者操作，禁止并发调用。`size()` 包含 Writing 与 Ready，用于停止排空判断；`has_ready()` 只表示当前存在可安全读取的 Ready payload。

### 4.4 monitor（src/monitor）— 见设计 §12

Breaker 状态机（核心），输入事件驱动，纯逻辑可测：

```cpp
enum class BreakerLevel : uint8_t { Normal, BrokenL1, BrokenL2, Safe, Recovering };

class Breaker {
public:
    explicit Breaker(const DefaultConfig& cfg) noexcept;
    // 事件输入
    void on_direct_timeout() noexcept;          // 连续 3 次 -> L1
    void on_dispatcher_rtc_timeout() noexcept;  // 连续 3 次 -> L2（隔离慢 AO）
    void on_watermark_violation() noexcept;     // 持续 >80% -> L2
    void on_overflow() noexcept;                // -> L2
    void on_key_reserve_exhausted() noexcept;   // -> Safe
    void on_watchdog() noexcept;                // -> Safe
    void on_dispatch_cycle() noexcept;          // 每派发周期计数（冷却）
    void on_probe_success() noexcept;
    void on_probe_failure() noexcept;           // Recovering -> L2
    void on_external_safe_restore() noexcept;   // Safe -> Recovering
    // 查询
    BreakerLevel level() const noexcept;
    bool direct_allowed(TargetId ao) const noexcept;   // L1 撤销该 AO direct
    bool healthy_window_passed() const noexcept;
};

template <typename Config = DefaultConfig>
class BreakerBank {
public:
    void on_direct_timeout(TargetId target) noexcept;
    void on_dispatcher_rtc_timeout(TargetId target) noexcept;
    void on_overflow(TargetId target) noexcept;
    BreakerLevel level(TargetId target) const noexcept;
    bool direct_allowed(TargetId target) const noexcept;
    bool drop_non_critical(TargetId target) const noexcept;
    bool safe_events_only(TargetId target) const noexcept;
    void broadcast_watermark(uint8_t percent) noexcept;
    void broadcast_watchdog() noexcept;
    void broadcast_dispatch_cycle() noexcept;
};
```

- 冷却默认 `kCooldownCycles`，恢复需冷却完成 + 低水位 + 无违规窗口 + 连续健康探针（设计 §12.4）。
- `Runtime` 使用固定容量的 per-AO `BreakerBank`；目标级超时、RTC 超时、overflow 与探针只更新对应 `TargetId`，系统水位、watchdog、调度周期及外部恢复通过显式 `broadcast_*` 广播。
- `BreakerBank` 对 `kInvalidTarget` 或超出 `Config::kMaxAo` 的目标采用 fail-safe：写操作忽略，`level()` 返回 `Safe`，`direct_allowed()` 返回 `false`，`drop_non_critical()` 与 `safe_events_only()` 返回 `true`，不得越界访问内部数组。
- `Monitor`：每 AO 与全局固定计数器（direct/dispatcher 时长、C1-C7 拒绝、分区水位、disposition 计数、lease 竞争、pending max）+ watchdog 心跳。热路径只写计数，不格式化。SMP 允许每 CPU 计数后汇总。

### 4.5 policy（src/policy）— 见设计 §11

```cpp
struct PolicyResult { bool accept; bool try_merge; uint16_t reason; };

struct PolicyOps {
    PolicyResult (*evaluate)(void* context, TargetId target,
                             const Event& event, const EventQos& qos, uint64_t now);
    bool (*merge)(void* context, Event& queued, const Event& incoming);
};
```

MergeCell（固定槽，状态机 `Empty/Published/Merging/Consuming`，CAS 原子转移）：
- 生产者 CAS `Published->Merging` 后改旧 payload，release 回 `Published`；
- Dispatcher CAS `Published->Consuming` 后取得 owning handle；
- 失败不等待，新事件进普通 staging；
- merge 只在事件类型显式声明时允许；有界、不可阻塞。`MergeCell` 持有一个已投递的 `Event*`（引用计数事件，依赖 event 模块），替换 payload 走 CAS 状态机。

### 4.6 ao（src/ao）— 见设计 §5

```cpp
enum class AoRunState : uint8_t { Idle, RunningDirect, RunningDispatcher };

class ExecutionLease {
public:
    bool try_acquire(AoRunState desired) noexcept;  // 原子 Idle -> desired，失败返回 false
    void release(AoRunState expected) noexcept;     // 校验 expected 后 -> Idle
    AoRunState state() const noexcept;
};

class PendingCounter {
public:
    uint16_t load() const noexcept;     // acquire 读
    void increment() noexcept;          // release 写（先于 queue publish）
    void decrement() noexcept;
};

class AoBase {
public:
    virtual void dispatch(const Event& event) noexcept = 0;
    virtual bool try_dispatch_queued(const Event& event) noexcept = 0;
    virtual LogicalPrio logical_prio() const noexcept = 0;
    virtual PriorityClass priority_class() const noexcept = 0;
    virtual bool direct_eligible() const noexcept = 0;
    virtual bool isr_direct_safe() const noexcept = 0;
    virtual ExecutionLease& lease() noexcept = 0;     // 补充：coordinator C5 访问
    virtual PendingCounter& pending() noexcept = 0;   // 补充：coordinator C4/C6 访问
protected:
    ~AoBase() noexcept = default;   // 非拥有基类：AO 静态/自动存储期，禁止经基类 delete
};

template <typename Context, typename Hsm, typename Traits>
class Ao : public AoBase {
    // Traits 提供：logical_prio/priority_class/direct_eligible/isr_direct_safe/kRtcBudgetNs
    // dispatch() 调用 hsm_.dispatch(context_, event)，保证 RTC 同步完成。
};
```

`AoRegistry`：定长 `AoBase*` 数组（**非拥有**，仅保存指针，永不 `delete`），`TargetId`（1 基）映射，`lookup(TargetId)`、`bind(AoBase*, prio)` 校验优先级唯一。AO 一律静态/自动存储期，生命周期由自身存储期决定，禁止经 `AoBase*` 释放。

**执行权（已定型，S6 落地）**：`Ao<Context,Hsm,Traits>::dispatch()` 是保留非法重入硬断言的自包含单执行权入口；`try_dispatch_queued()` 使用同一 `Idle→RunningDispatcher` CAS，但 lease 忙时返回 false 且不触碰事件。Dispatcher 只走 `try_dispatch_queued()`：失败时最多保留一个 deferred slot，不减 pending、不释放事件；睡眠前执行 `arm→CAS 重试→PAL wait`，direct 在释放 `RunningDirect` 后若 `pending>0` 请求唤醒。stop drain 对 deferred slot 与队列事件分别恰减一次 pending、恰释放一次引用。Direct 路径走 `dispatch_direct()`，同样由 AO 内部持有 lease。

### 4.7 staging（src/staging）— 见设计 §10

三区异构容量 staging，每个分区**独立类型/容量**（不能用同一模板容量冒充）：

```cpp
struct StagingSlot {
    TargetId target;
    Event* event;          // 已转移的 owned reference；dispatcher 完成后 event_gc
    uint64_t enqueue_ns;   // 用于 Low 老化计时
};

// 纯批处理逻辑：优先级顺序 + aging 例外 + 数量上界（BatchSizeMax）
class BatchSelector { /* High->Normal->Low；Low 超 LowMaxWaitMs 强制取一 */ };

// 三区统一视图，编译期绑定队列后端（Mpsc 或 CriticalRing）
template <typename Config, typename QueueBackend>
class Staging {
public:
    // 每 AO 固定 PriorityClass 决定分区；watermark 返回 0-100 使用率
    bool enqueue(TargetId target, Event* e, PriorityClass cls, uint64_t now_ns) noexcept;
    bool dequeue_one(StagingSlot& out) noexcept;   // 按优先级/aging 取一个
    uint8_t watermark(Partition p) const noexcept; // 50/80/95 档位
    uint16_t size(Partition p) const noexcept;
};
```

注意：staging 是数据结构 + 批处理选择逻辑；**Dispatcher 线程循环放 core 模块**（依赖 PAL wait/signal）。

### 4.8 core（集成）— 见设计 §4/8/13

- `dispatcher.hpp`：`Dispatcher`（单线程循环：drain → 取 batch → dispatch → 释放；空闲时以 acq_rel exchange 清 wake latch、复查 Ready，再调用 PAL wait）。producer 在 publish 后以 exchange 置 latch，仅 false→true 的首个 producer 发 signal，避免 missed wakeup 与逐 submit 唤醒。停止先关闭 submission admission，再等待已获 lease 的 submit 完成；Writing 槽计入 buffered，排空只读取 Ready 槽，最后一个 submission lease 负责唤醒停止等待。
- `coordinator.hpp`：`DispatchCoordinator::submit_from_task(TargetId, Event*, const EventQos&) / try_submit_from_isr(...)`（事件引用由 submit 管理：入队则保留 ref 待 dispatcher gc，direct 则处理完 gc，drop/merge 则立即 gc），按设计 §8.1 管线：M4 → M1(C1-C7) → direct | merge | staging。统一入口，禁止绕过。
- `runtime.hpp`：`Runtime<Config, Pal, Profile=HostSmpProfile>`：`initialize/bind/start/run_dispatcher/stop` 三阶段初始化。`Runtime` 显式使用 per-AO `BreakerBank<Config>`；通用 `DispatchCoordinator`/`Dispatcher` 模板默认仍为单 `Breaker<Config>`，需要 per-AO 隔离时显式传入 Bank。默认 16 AO 的 Bank 约占 128 B，相对单 `Breaker` 净增约 120 B。`start()` 返回 `bool`，只有 PAL 报告 Dispatcher 启动成功才进入 started（design §7.5）；第三模板参把板级 profile 传导到 Dispatcher（单核 `RttSingleCoreProfile`→immediate reclaim，Host 默认→batched）。
- `src/core/pal_posix.cpp`：`pal::Posix`（pthread、condvar、`clock_gettime(CLOCK_MONOTONIC)`）。
- `src/core/pal_rtthread.cpp`：`pal::RtThread` 静态 PAL（design §7.5）：调用方提供 `RtThreadResources<StackBytes,ContextSlots>`（静态 TCB、对齐 stack、两个静态 semaphore、固定 `ContextSlot[N]`）；构造函数只保存引用；显式 `initialize()` 返回 `pal::InitError`；`start_dispatcher()` 返回 `pal::InitError`（只有 `rt_thread_startup()==RT_EOK` 才 kOk）；一次初始化/一次启动/一次停止，stop 后再次 start 返回 `kAlreadyStarted`；固定 ContextSlot 表不占 `user_data`、启动后冻结、Dispatcher 经静态 TCB 比较识别；`ClockOps` 静态函数表注入时钟（默认 RT tick，真机绑 10 MHz TIM）。
- `src/core/test_integration.cpp`：端到端测试（生产→submit→dispatcher→AO action）。

## 5. 验收标准（每个模块）

1. `cmake -B build_<mod> -S .` 无错误，`-fno-exceptions -fno-rtti` 生效。
2. 所有测试通过（`ctest --output-on-failure`）。
3. 每个不变量至少一个负例测试（如：double-wrap 检测、move 后访问空、队列满返回 false、lease 冲突失败、breaker 恢复需连续健康窗口）。
4. 无动态分配证据：allocator hook 在 Running 后路径无堆调用（event/staging/core 模块）。
5. 报告中列出：实现文件、测试清单、与契约的偏差、未决问题。
