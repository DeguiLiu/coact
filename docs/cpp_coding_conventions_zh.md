# coact C++ 编码规约（中文版）

本文档是 coact 仓库 C++ 代码的完整编码规约，视角是**现代 C++17**——不是从 C/MISRA 移植过来的 C-with-classes，而是编译期确定、零拷贝、低分支、静态多态优先的 C++17 表达。目标读者：在本仓库编写或评审 C++ 代码的工程师与自动化审查代理。每条规约均写成可判定形式——评审时对每条回答"是/否"即可，不存在模糊表述。代码落点标注到文件与类名/函数名，不标注行号（行号随重构漂移）。

规约来源分两层：

- 框架层：`include/coact/` 头文件（`ao.hpp`、`hsm.hpp`、`pool.hpp`、`coordinator.hpp`、`runtime.hpp`、`queue.hpp`、`expected.hpp`、`static_ao.hpp` 等）；
- 示例层：`examples/isp_pipeline_demo.cpp`（本仓库最大的 C++ 落点，约 4400 行，本文绝大多数代码落点取自该文件），辅以 `examples/log_rtthread_demo.cpp`、`examples/node_manager_demo.cpp`。

适用范围：`include/coact/`、`examples/`、`src/`、`test/` 下全部 C++ 代码。PAL 的 RT-Thread 适配层确实内含少量 C 接口（rt_kprintf 等），这些只在 7.2"平台适配层注意"小节收口；正文全部是 C++17 规约。

红线速查（详见各章）：禁止异常；禁止 `new/delete` 运行期堆分配（业务代码零堆）；禁止裸 `int/long/char`；禁止裸 `enum` 与 `#define` 常量；禁止裸指针代替引用/句柄（原始内存槽位除外，且须 launder 护住）；禁止动态分配池 / 工厂模式 / 过度抽象层；禁止跨 AO 共享可变状态；禁止 Dispatcher 上下文自提交会话级广播；禁止非平凡类型 placement new 进复用内存；禁止固定 sleep 排空；禁止为用而用任何 C++17 特性；三个相似才抽基类；组合优先于继承，静态多态优先于运行时多态。

## 1. 总则

### 1.1 语言与标准

- **C++17**。禁止使用 C++20 特性（concepts、ranges、`<format>` 等）。理由：双平台工具链均以 C++17 为稳定基线。
- 可用的 C++17 核心能力以第 5 章的使用规约为准；语言特性本身允许不等于任何场景都该用。

### 1.2 双平台约束

- 所有 C++ 代码必须同时可在 **RT-Thread 目标机** 与 **Linux host** 编译运行。平台差异只允许通过 `include/coact/pal.hpp` / `pal_posix.hpp` / `pal_rtthread.hpp` 的 PAL 接口隔离，禁止在业务代码里出现 `#ifdef` 平台分支。
  - 落点：`Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>`（isp_pipeline_demo.cpp）；RT-Thread 侧对应 `pal_rtthread.hpp`。
- 池等共享结构通过 **Profile 模板参数** 区分单核/SMP 语义，而不是 if/else：
  - 落点：`coact::RttSingleCoreProfile`（irq-mask 临界区，无 CAS）与 `coact::HostSmpProfile`（32 位 tagged 原子头 + CAS），见 `include/coact/pool.hpp` 头部注释；Profile 合法性由 `EventPool` 内 `static_assert` 把关。

### 1.3 现代 C++17 基调

本仓库的 C++ 有四条基调，全文档各章都是它们的具体化：

- **编译期确定、运行期少分配**：能在编译期定型的（表、策略、布局契约、AO 属性）一律 constexpr/模板/static_assert；运行期只留下"数据流动"本身，业务代码零堆分配。
- **零拷贝**：对象在目的地就地构造（placement new + 对齐存储），事件只传描述符不传像素字节；所有权移动用 `std::move` / `std::exchange`，不用复制。
- **低分支**：编译期路径分流（`if constexpr`）+ 表驱动（状态表、命令表）取代 if/else 链；热点循环内不引入可被 Profile 消除的分支。
- **边界清晰**：每个跨模块结构体在定义处用 `static_assert` 类型萃取声明契约；每个并发边界（AO 间、worker 交接）只经事件平面；违反任何边界 = 编译失败或显式 reject 弧，绝不静默。

### 1.4 语言与注释语言

- 代码、注释、commit message 用英文；面向人的文档（本文件）用中文。
- 禁止在代码注释中夹杂中文（现有示例中"花屏/丢帧/闪屏"等业务术语注释为存量，新增代码不再扩展此风格）。

## 2. 类型与内存安全

### 2.1 类型纪律（现代 C++ 表达）

- **固定宽度整型**：优先 `<cstdint>` 体系（`uint8_t / uint16_t / uint32_t / int8_t / int32_t`），禁裸 `int / long / char / unsigned`；并把"该类型宽度即契约"交给类型萃取把关（`sizeof`/`is_standard_layout` 断言），不靠人肉记忆。
  - 落点：`IoMeta`、`FrameGeometry`、`Payload`（isp_pipeline_demo.cpp）全部字段均为固定宽度；`kDdrCount <= 7U` 的 `static_assert` 把宽度契约钉在定义处。
- **强类型代替弱转换**：领域枚举用 `enum class X : 底层类型`（禁 `#define` 常量、禁裸 `enum`）；可判空的语义类型用 `explicit operator bool()` 而不是返回裸 int/指针；错误用 `Expected`/错误码枚举而不是裸 int 返回。
  - 落点：`enum class Sig : uint16_t`（事件词汇表）、`enum class SessionState : uint8_t`、`enum class RecfgStage : uint8_t`、`enum class SrMagx : uint8_t`；框架层 `enum class TransitionKind : uint8_t`（hsm.hpp）、`enum class PriorityClass : uint8_t`（config.hpp）；`AddrCache::explicit operator bool()`。
- **隐式转换显式标注**：任何跨宽度/跨符号赋值必须写 `static_cast<目标类型>(...)`。
  - 落点：`static_cast<uint16_t>(frame_id % kDdrSlots)`（`dn_slot_of`）；`static_cast<uint16_t>(Sig::kFrameIrscOut)` 等全文件一致使用。
- 常量左侧（Yoda 比较）：`0 == x` / `nullptr == p`。
  - 落点：`if (nullptr == ack) { return; }`（`onIrscDmaDone`）、`if (nullptr != done)`（`CmdDmaWorker::execute`）、`(SessionState::kInit == s)`（`irsc_session_open`）。

### 2.2 健壮性基础（源自 MISRA 精神的现代等价物）

MISRA C:2012 在本仓库不再逐条适用；它的精神已翻译成 C++17 表达：

- 单语句也必须 `{}`——brace-init 与花括号纪律在 C++ 里同样防 dangling-else。
  - 落点：全仓库一致；如 `if (nullptr != e) { ... }` 的每个分支。
- **禁 `goto`**：控制流跳转由 RAII（析构保证清理）、HSM 拓扑（转移表）和提前 return 承担。
- **禁递归**：组合/树遍历用编译期固定深度或展平循环（见 6.4）。
- `switch` 必须 `default` 或穷举 + 兜底返回——编译器对 `enum class` 的穷举警告配合使用。
  - 落点：`session_state_name` 显式 `default:`；`recfg_stage_name` 穷举后 `return "?"`。
- 手工配对释放（malloc/free、lock/unlock 的裸调用）**整体被 RAII 取代**：成对操作包成 guard 对象，任意退出路径（含提前 return）自动配对。
  - 落点：`BypassGuard`（RAII 装饰器，进入置 `cache_bypass`、析构 resync——"an early return cannot skip the resync"）；框架层 `CriticalSection::Token`（queue.hpp `BoundedMpscQueue::try_push_observed` 中临界区 token 的取用即释放模型）。

### 2.3 内存策略：编译期确定、运行期零堆

- **禁止动态分配池/工厂模式**。需要"多形态行为"时用模板参数化（CRTP/Policy 静态多态）或 `constexpr` 数据表；确需运行期擦除的少量场景用 const 函数指针表，但首选模板。
  - 落点：`kIrscDoneTable`、`kProductModes`、`kCaliModes`、`kSigNames` 均为 `constexpr` 数据表（const 表代替 C 静态表思维的现代形态：`constexpr` 使其可进编译期消费）；行为多态走 `GainNodeBase<Policy>` 模板；框架层 `coact::StaticAoEntry` + `make_static_ao_entry`（static_ao.hpp）是全仓库唯一的函数指针擦除点，且注释声明"deliberately does not replace AoBase ... until target measurements prove a benefit"。
- **静态/栈上存储优先，业务代码零堆**：事件池存储由调用方提供对齐的静态数组，池只做管理；容器一律编译期容量（模板参数），不 `new`/`malloc`。
  - 落点：main 中 `alignas(kPayloadAlign) std::array<uint8_t, sizeof(Layout) * 128U + kPayloadAlign> storage{}` + `pool.init(storage.data(), storage.size(), coact::make_critical_section(pal))`；框架层 `BoundedMpscQueue` 的 `Capacity` 模板参数与 `SlotStorage`（queue.hpp）、`Expected<V,E>` 的 fixed inline storage（"no exceptions, no heap"，expected.hpp）。
- **侵入式链表代替堆容器**：排队结构用 `next_pending` 侵入式指针。
  - 落点：`RecfgCtx::next_pending`（注释 "intrusive queue (static, no heap)"）。
- **减少裸指针 → 引用/句柄**：所有权清晰的传参用引用（`const Layout&`、`VideoPathMirror&`），跨线程/跨边界的可空句柄用 `TargetId`/`pool_id` 等值语义 id，不用裸指针 + 注释描述谁拥有。`reinterpret_cast` 只允许出现在"原始内存槽位 ↔ 对象"的 launder 桥两侧（见 2.4）。
  - 落点：`IrscDriverAo::context().cmd_channel` 等跨边界连接全部走 `TargetId` 值；框架层 `submit_from_task(TargetId, Event*, ...)` 的接口形态。

### 2.4 placement new 与对象生命周期纪律

- **placement new 仅用于平凡可析构类型**，且必须由 `static_assert(std::is_trivially_destructible<...>)` 或 `is_trivially_copyable` 在定义处护住；读侧必须经 `std::launder` 重建指针-对象关系（C++17 对象模型要求）。
  - 落点（写侧）：
    - `DdrCtx::write`——`::new (static_cast<void*>(slot_mem)) FrameStamp{...}`：DDR 槽位内直接构造 DMA 描述符式头部，无临时对象无拷贝；
    - `WorkerBase::submit`——`::new (static_cast<void*>(&slots[head])) Job(j)`：worker 环形槽位原地构造；
    - `AddrCache::store`——`::new (static_cast<void*>(&rec)) AddrRecord{...}`；
    - 框架层 `EventPool`（pool.hpp）——`::new (block) Event{...}` 与 `alloc_typed` 的 `::new (block) Layout{}` / payload 构造。
  - 落点（读侧）：`DdrCtx::read` 与 `WorkerBase::run` 中 `*std::launder(reinterpret_cast<const ...*>(slot))`。
- **零拷贝**：placement new 是"目的地构造"的唯一手段——对象生命周期直接开始在池块/DDR 槽/缓存槽的内存上，无临时对象、无拷贝。配合 `std::launder` 读侧，构成 C++17 对象模型下的完整零拷贝通道。
- 跨模块边界或进入池/DDR 内存的每个结构体，必须在**定义处**用 `static_assert` 声明 ABI/布局契约：`is_standard_layout` + `is_trivially_copyable` + `is_nothrow_move_constructible`。
  - 落点：`FrameStamp`、`FrameGeometry`、`IoMeta`、`VideoPathMirror`、`VideoCtx`、`VideoPackCtx` 各自紧跟定义的 `static_assert` 块（"Every struct that crosses a module boundary ... is constrained HERE"）。
- **`std::byte` + `alignas` 是原始内存的正字标记**：未构造的原始存储用 `std::byte` 数组（不是 `char`/`uint8_t`），并在存储类型上 `alignas(alignof(T))`；访问一律经 placement new + launder 桥，不经裸类型双关。
  - 落点：框架层 `SlotStorage<T>`（queue.hpp，`alignas(alignof(T)) std::byte bytes[sizeof(T)]` + `slot_ptr` launder 访问器）；`Expected<V,E>::Storage`（expected.hpp，同构）；`EventBlockLayout` 的 `alignas(PayloadAlign) std::byte payload[PayloadBytes]`（pool.hpp）。
- **move 语义即所有权语言**：跨线程/跨窗口交接用 `T&&` + `std::move`（框架层 `BoundedMpscQueue::try_push_observed(T&&)`——payload 只在容量确认后 move 一次，"A failed push does NOT consume the caller's value"）；"取旧+置新"一体语义用 `std::exchange`（见 5.6）。对象必须 `is_nothrow_move_constructible` 才允许进这些通道（框架层 `Expected` 对 `V` 的构造前置断言即此纪律）。

## 3. 线程与并发

### 3.1 AO 事件平面（Active Object）

- AO 之间**只通过事件通信**：`pool.alloc_typed` 分配事件块 → `rt.coordinator().submit_from_task(target, &e->event, ...)` 投递。禁止跨 AO 共享可变状态。
  - 落点：isp_pipeline_demo.cpp 每个 action 函数末尾的标准三步（alloc → 填 meta/payload → submit）。
- 非 AO 线程（pthread worker、ISR 模拟）与 AO 的**唯一耦合是事件平面**；worker 不读 AO 内部字段。
  - 落点：`IrscWorker`、`UsbDmaWorker`、`WorkerBase` 派生族；文件头注释 "Each worker's ONLY coupling with the AOs is the event plane"。
- **数据平面（像素字节）不走事件**：事件只携带缓冲描述符（slot id + region id），真实数据留在 DDR 由拥有者 AO 管理。
  - 落点：`Payload::ddr_slot/ddr_id` + `DdrCtx` 区域；注释 "the payload carries only the buffer descriptor"。

### 3.2 锁层级与最弱足够同步

- 锁层级 **L1 Singleton → L2 Context → L3 Device**，禁止反向获取。
- **最弱足够原则**：若数据已被互斥机制（单 Dispatcher 线程序列化、AO 队列、单写者）覆盖，**不加锁**，且必须写注释论证为什么不需要锁。
  - 落点：`DdrCtx` 定义处注释——"Because the coact Dispatcher is single-threaded, all node actions run serially, so these accesses need no lock"；main 中 `Single DDR owner ... serialized), so no lock, no blackboard`。
- 模拟硬件寄存器的全局（`g_zoom` / `g_sel` / `g_wrape`）允许存在，但必须**单写者**（一个 AO action 写）+ 同 Dispatcher 线程读 + 变更经事件传播，并写明它不是黑板。黑板（多生产者 + 轮询消费者）仅在多传感器融合类需求下允许。
  - 落点："WHY THIS IS NOT A BLACKBOARD" 注释块。
- 跨线程枚举/标志用 `std::atomic` 且必须断言 `is_always_lock_free`（libatomic 回退是隐藏的锁/堆依赖，构建期必须失败）。
  - 落点：`static_assert(std::atomic<SessionState>::is_always_lock_free, ...)`（`g_session`）、`std::atomic<uint16_t/bool/uint32_t>::is_always_lock_free` 三连断言。

### 3.3 Worker 交接契约：单槽、忙则拒绝 + 计数

- **worker 单槽/浅环交接**：`submit()` 满则返回 false，**不阻塞、不排队**；调用侧 AO 计数丢弃（`rejected` / `channel_rejects` / `tx_rejects`）。丢帧是诚实的计数器，不是隐藏的停顿。
  - 落点：`WorkerBase::submit`（ring 满返回 false）+ `executed_count()/rejected_count()` 原子计数；`UsbDmaWorker::submit`（单槽 `job.valid` 即拒绝）；调用侧 `onIrscCmd`、`VideoPackNode::on_input`、`onTempPacked`。
- **drain-on-stop**：`stop()` 先排空在途任务再退出（off-by-zero 停机契约：每个被接受的 job 必须产出完成事件）；与 `UsbDmaWorker` 的丢弃语义并存时**两者都要注释写明差异**。
  - 落点：`WorkerBase::stop`（idle_cv 等待 + join）及 "In-flight jobs are NOT discarded (unlike UsbDmaWorker)" 注释；main 的 worker 停机顺序注释（先 drain 喂 AO 的通道，最后 `rt.stop()`）。
- 关键区只含少量 store，**绝不把硬件延迟圈进锁内**；耗时操作在锁外执行。

## 4. 函数与控制流

### 4.1 return 预算

- **单个函数的 return 语句不超过 5 个**；能不提前 return 就不提前 return。错误路径可提前返回，但超过 5 个 return 说明函数职责过多，应拆分。
  - 落点：`onIspCmd`、`HlCtx::maybe_fuse`（2 个前置 guard + 主路径）、`onIrscDmaDone`（1 个 guard）。

### 4.2 guard / entry / exit / action 分层

- **guard 是纯函数**：只读 ctx 与 event、无副作用、`noexcept`、返回 bool。副作用禁止出现在 guard 里。
  - 落点：`irsc_session_open`、`cmd_is_pic` / `cmd_is_start` 系列、`at_precheck` / `at_quiesce` / `at_resume`、`sout_is_pic` / `sout_is_temp`。
- **entry 只做硬件命令**：进入状态时发出的硬件操作（寄存器写、启停命令）必须放在 entry action，**不得放在 transition action**（transition action 在状态切换前执行，从那里自提交事件会与拓扑竞争）。
  - 落点：`rcQuiescingEntry`（SOUT_STOP_FRAME）与 `rcResumeEntry`（SOUT_START），以及 `rcEnterPrecheck` 中 "SOUT_STOP_FRAME is issued by the Quiescing ENTRY action ... self-submitting here would race the topology" 的论证注释。
- **exit 只做清理**：退出状态时的资源释放/计数收尾。框架由 `Hsm::exit_to_lca`（hsm.hpp）按状态栈逐层调用 `StateDef::exit`。
- **action 是事件响应**：transition action 只做"本弧的业务效果 + 更新镜像枚举 + 链接下一事件"，控制流归 HSM 拓扑。
  - 落点：`rcEnterApply` / `rcEnterSync` / `rcEnterCommit`（每函数一阶段工作）。

### 4.3 状态机表驱动

- AO 行为一律**静态 HSM 表**驱动（`StateDef[]` + `TransitionDef[]`），禁止在 action 里 if-else 模拟状态机。
- 拒绝路径必须显式：非法 (状态, 事件) 对落到 Self 转移的 reject 弧（计数 + trace），禁止静默丢弃。
  - 落点：`onVideoCmdRejected`、`onIrscCmdRejected` 与 `kVideoTransitions` 尾部的 reject 弧组。

### 4.4 事件生命周期与运行期监控

- 事件块由池分配（`alloc_typed`）后，引用计数（`Event::ref_ctr`，event.hpp）由框架管理：alloc 后为 1，每多投递一次 +1，归 0 回收。业务代码**只投递（submit）不手动回收**；demo 结束必须断言 `pool.used() == 0U`（零泄漏）。
  - 落点：isp_pipeline_demo.cpp verification 块 "event pool fully reclaimed"。
- AO 静态属性（优先级、RTC 预算、直投资格）一律走 **Trait** 结构体（`AoTrait<Prio>`、`VideoFsmTrait`、`IrscTrait`），不通过构造参数或运行期 setter。
  - 落点：`AoTrait` 模板（`logical_prio` / `priority_class` / `direct_eligible` / `isr_direct_safe` / `kRtcBudgetNs`）；框架层 `coact::Ao<Ctx, Hsm, Trait>` 三参数形态（ao.hpp）。
- 运行期可观测性来自 monitor（`rtc_timeouts` / `disposition_overload` / `pending()` 计数器），不往业务代码里加打印探针。
  - 落点：main 中 `rt.monitor().ao(kVideoFsmId).rtc_timeouts` 等；`include/coact/monitor.hpp`。
- HSM 转移种类用对：`TransitionKind::Internal`（只跑 action）、`Self`（拒收/自环）、`External`（离开子树再入目标，边界状态重跑 entry/exit）。
  - 落点：`include/coact/hsm.hpp` 的 `TransitionKind` 三值定义及 `Hsm` 分派 switch；demo 中 guarded accept 用 Internal、reject 用 Self、跨阶段推进用 External。
- 排空（drain）用事件驱动条件（每 AO `pending().load()` 归零 + FSM 回到终态），**禁止固定 sleep 赌时序**（demo 注释记载 stress 中一次竞态教训）。
  - 落点：main 两处 drain 循环（"Pending-based drain is the correct contract"）。
- 主线程提交会与 Dispatcher 竞争唤醒闩锁的场景（会话状态广播），必须从**主线程**发起而非 Dispatcher 上下文自提交，并注释论证。
  - 落点：`request_recfg` lambda 中 "must never be submitted from the Dispatcher thread" 注释。
- AO 上限由配置约束（`kMaxAo = 16`，config.hpp），AO 合并（如两个流并入一个 HSM 的状态乘积）是达标手段；合并后状态命名编码各流相位。
  - 落点：`kVfII..kVfGT`（PIC × TEMP 9 态乘积表）与 `kPackBA..kPackBS`（2×2 写回窗口乘积）及 "closed product table" 注释。

## 5. C++17 特性使用规约

每特性三段式：何时用 / 红线 / 代码落点。

### 5.1 `if constexpr`

- **何时用**：同一模板需要按编译期条件实例化不同路径，且不需要的那条路径根本不该被实例化（依赖不存在、或想消除运行期分支）。
- **红线**：禁止用 `if constexpr` 包裹恒真/恒假条件来"预留未来分支"（除注释明示的示范代码）；运行期才知道的条件必须用普通 `if`。
- **落点**：`QuiescePolicy<HasIdleIrq>::quiesce`——有中断源实例化 `EventQuiesce`，无中断源实例化 `PollQuiesce`（"the poll loop code is not even instantiated"）；`EventPool` 内 `if constexpr (detail::is_single_core_pool_profile_v<Profile>)`（pool.hpp）按 Profile 分流单核/SMP 实现；`pal_rtthread.hpp` 的 tick 换算按 `RT_TICK_PER_SECOND` 分流。

### 5.2 `constexpr` / `inline constexpr`

- **何时用**：模式表、命令表、调参常量、枚举名表——所有"数据即契约"的常量集合（`enum class` + `constexpr` 表共同取代 `#define` 常量）。头文件内表用 `inline constexpr` 避免 ODR。编译期确定的值一旦被 `static_assert` 或模板消费，即锁进二进制。
- **红线**：不为 constexpr 而 constexpr（运行期才确定的值就用普通变量）；禁止把大表写成 constexpr 但从未被编译期消费（如 static_assert 或模板）又声称收益。
- **落点**：`kProductModes` / `kCaliModes`（模式表）、`kIrscCmdSequence`（启动命令表，"order IS the contract"）、`kIrscDoneTable`、`kLat`、`kSigNames`、`inline constexpr bool kFmtHasIdleIrq`（硬件能力开关）；框架层 `inline constexpr uint16_t kReclaimBatchCap`（pool.hpp）、`reclaimer_pool_capacity()`（constexpr 函数）、`kMaxAo`（config.hpp）。

### 5.3 `enum class` + 底层类型

- **何时用**：一切新枚举；跨边界/进事件的枚举必须带底层类型。
- **红线**：禁止裸 `enum`（无作用域枚举）用于新代码；存量裸枚举（`IrscStep`、`VideoCmd`、`DdrId`）仅因与 `uint8_t/uint16_t` 字段直接互转而保留，不得新增。
- **落点**：见 2.1 第三条清单。

### 5.4 `static_assert` 类型萃取

- **何时用**：凡是"该类型必须满足 X"的假设，一律在定义处或模板内断言（`is_standard_layout` / `is_trivially_copyable` / `is_nothrow_move_constructible` / `atomic<T>::is_always_lock_free` / `is_same`）。违约必须编译失败，不许到现场才炸。
- **红线**：禁止断言显然为真的平凡事实凑数（如 `static_assert(sizeof(char) == 1)`）。
- **落点**：isp_pipeline_demo.cpp "Compile-time ABI / layout / move-behavior contracts" 块（10 余条断言、每条带失败原因文案）；`EventPool` 内 Profile 合法性与 CAS lock-free 断言（pool.hpp）；T37 证据断言 `static_assert(kX1FrameBytes == 655360U, ...)`。

### 5.5 `[[nodiscard]]` / `noexcept` / `explicit`

- **何时用**：
  - `[[nodiscard]]`：返回值承载错误/所有权/关键结果的函数（忽略即 bug）；整个错误类型可直接 `class [[nodiscard]] Expected final`（框架层先例）；
  - `noexcept`：不抛函数（move、纯查询、guard、静态策略）——本仓库禁异常，`noexcept` 是接口契约而非优化提示；
  - `explicit`：单参构造与转换运算符（`explicit operator bool()` 让"有值"判断不会静默变 int）。
- **红线**：不整文件机械标注；`[[nodiscard]]` 用于"忽略它必然是错"的场景。
- **落点**：`class [[nodiscard]] Expected final`（expected.hpp，整类标注）；`[[nodiscard]] constexpr bool magx_supported(...)`、`FrameGeometry::bytes_per_frame`、`select_cmd`、`VideoCtx::mirror_of`、`AddrCache::query`；`explicit operator bool()`（AddrCache 与 Expected）；`explicit BypassGuard(PeriphRegCache&)`；`noexcept` 遍布 policy/guard/静态钩子。

### 5.6 `std::exchange`

- **何时用**：仅当需要**"取旧值 + 置新值"一体的原子语义交接**——即旧值确实被消费（移走、打印、作为提交值），且置新值是交接的一部分。所有权跨窗口移动（AO ctx 槽位 → 出向事件）是典型场景。
- **红线**：单纯赋值不得硬改成 `std::exchange`；不消费返回值时写 `static_cast<void>(std::exchange(...))` 并保留注释，证明旧值曾被有意丢弃。
- **落点**（isp_pipeline_demo.cpp 全文件 14 处，代码态 9 处）：`Job j = std::exchange(job, Job{})`（`UsbDmaWorker::run`，取走任务重置槽位）；`done->meta = std::exchange(ctx.pending_meta, IoMeta{})` + `std::exchange(ctx.pending_slot, 0U)`（`FusedNodeBase::complete_irq`，IRQ 窗口所有权移出）；`std::exchange(ctx.pic_pending, IoMeta{})` / `temp_pending`（`PicPackNode::on_sout_done` / `TempPackNode::on_sout_done`）；`const uint32_t previous = std::exchange(ctx.layout_version, ctx.layout_version + 1U)`（`rcEnterSync`）；`ctx.active_magx = std::exchange(ctx.target_magx, ...)`、`active_geom`（`rcEnterCommit`，最后提交）；`static_cast<void>(std::exchange(r.slot_frame[slot], ...))`（`DdrCtx::write`，弃旧值的规范写法）。

### 5.7 placement new + 对齐存储

- **何时用**：在原始内存（事件池块、DDR 槽、静态缓存槽）中就地构造，见 2.4。对齐由 `alignas`/对齐常量在**存储声明处**保证，不在使用处补救。
- **红线**：非平凡可析构类型禁止 placement new 进复用内存；禁止 placement new 后不经 `std::launder` 直接 `reinterpret_cast` 读。
- **落点**：`kPayloadAlign = 64U` + `EventBlockLayout<IoMeta, sizeof(Payload), kPayloadAlign>` + main 的 `alignas(kPayloadAlign) storage`；其余见 2.4。

### 5.8 `std::move` / 右值引用（值类别即所有权）

- **何时用**：跨线程/跨槽位交接对象时以 `T&&` 参数 + `std::move` 表达"源从此失效"；失败路径不得消费调用者的值（先查容量再 move）。
- **红线**：move 后的源对象禁止再读（源即已交接）；禁止对 const 对象强行 `const_cast` 后 move；复制成本可忽略的标量/描述符不必 move（过度 move 与漏 move 同罪）。
- **落点**：`BoundedMpscQueue::try_push_observed(T&&)` + `::new (slot) T(std::move(v))`（queue.hpp，容量确认后才 move）；`Expected` 的 move 构造/赋值（expected.hpp，move-only payload 的正字标记——拷贝被 `= delete`）。

### 5.9 特性使用总红线

- **不为用而用**：任何特性必须能回答"不用它会怎样"。
- **三个相似才抽基类**：出现第三处结构相似时才提取（CRTP/Policy/宏），两处时容忍重复。
- **禁过度设计**：helper / util / 抽象层最小化；宁可局部直白，不要全局优雅。
- 三处以上相似的 HSM 表可用宏压缩（`COACT_HSM_STATES` / `COACT_HSM_TRANS`），但宏必须保持"表即数据"（只拼表项，不嵌控制流），用后 `#undef`（见 `VF_ARC` ... `#undef VF_ARC`）。

## 6. 设计模式使用边界

四个允许的模式，各自有明确的准入条件与红线。通用红线：**每个模式引入前必须能指出"三个相似实例"或等价的复用证据**（`WorkerBase` 服务 5 个 worker；`VideoFsmNode` 服务 2 个流 + 已知第三流在路上属临界情况，需注释说明）。

### 6.1 CRTP（骨架 + 钩子）

- **准入**：一个骨架类承载固定生命周期/流程（启停、环、计数、阶段流），各派生类只提供少量钩子；需要编译期分发、拒绝 vtable。
- **红线**：钩子数量失控（>7 个）说明骨架在猜未来，退回普通函数组合；CRTP 基类不得持有 per-instance 状态（静态钩子风格时）。
- **落点**：
  - `WorkerBase<Derived, Job, kDepthV>`：互斥交接环 + drain-on-stop + executed/rejected 计数；`derived()` 经 `static_cast<Derived*>(this)` 编译期调 `execute` 钩子；派生 `CmdDmaWorker` / `IspIrqWorker` / `SoutDmaWorker` / `MipiIrqWorker` 各只写 `execute`。
  - `VideoFsmNode<PicFsmNode>` / `VideoFsmNode<TempFsmNode>`：静态钩子（`mirror_of` / `label` / `kind_value` / `ack`）。
  - `VideoPackNode<PicPackNode>` / `VideoPackNode<TempPackNode>`：钩子面为 `kPathId/in_region/pack_latency_us/pack/account/park/count_sub/count_reject/on_sout_done`（该文件最大钩子面，处于红线内但不再扩）。
  - 框架层 `make_static_ao_entry`（static_ao.hpp）：captureless lambda 擦除 dispatch 调用，const 函数指针表。

### 6.2 策略（Policy，算法族参数化）

- **准入**：同一算法骨架 × 可替换的无状态算法，策略是**只有静态 `apply()`（或等价静态方法）的无状态 struct**——`std::allocator` / `std::hash` 的定制点风格。
- **红线**：策略禁止携带状态（有状态策略改用 CRTP 或独立类）；策略方法必须 `noexcept`（可 `constexpr`）。
- **落点**：`LowGainPolicy::apply` / `HighGainPolicy::apply` + `GainNodeBase<Policy>`（`using LowGainNode = GainNodeBase<LowGainPolicy>`）；`EnhancePolicy` / `TempPolicy` + `FusedNodeBase<Policy>`；`EventQuiesce::wait` / `PollQuiesce::wait` + `QuiescePolicy<HasIdleIrq>` 门面；`StaleFraming::frame` / `AlignedFraming::frame`。

### 6.3 命令（延迟执行 / 顺序契约）

- **准入**：(a) 操作需要携带自身身份/参数延迟投递执行；或 (b) 操作序列的**顺序本身是契约**，必须以数据表形式固化可审。
- **红线**：命令对象必须自包含（自带 tag/参数，不依赖调用点上下文）；禁止把命令表当成变相 if 链（每个命令一个几乎相同的 handler）。
- **落点**：`IrscCmd`（step + tag + `stamp(Layout&)` 把身份写进事件块）+ `constexpr IrscCmd kIrscCmdSequence[]`（init → start → ctrl → output_enable，"order IS the contract"）；完成姿态表 `kIrscDoneStep kIrscDoneTable[]` + `irsc_done_action_of(step)`（"the table, not an if-chain, is the driver's semantics"——表驱动取代 if 链是正面示例）。

### 6.4 组合（树形结构传播）

- **准入**：需要向固定成员集合传播同一操作（init/deinit/状态广播）。
- **红线**：**编译期固定成员、禁递归**——组合被展平为固定数组的普通循环（demo 明确"flattened to plain iteration (no recursion; the tree has exactly two levels)"）；成员集合运行期不可变。
- **落点**：`SessionEventComposite`——固定 `TargetId targets[kMaxTargets]` 数组，boot 期 `add` 一次，之后 `publish()` 顺序迭代广播（每个观察者经自身 AO 队列序列化，无需锁）；init 正向 / deinit 逆向的顺序契约另见 `VideoFsmNode::on_init`（正序循环）与 `on_deinit`（`for (int8_t i = count-1; i >= 0; --i)` 逆序循环）。

### 6.5 模式选择决策表

| 场景特征 | 选 CRTP | 选策略 | 选命令 | 选组合 |
|---|---|---|---|---|
| 差异是**钩子方法集合**（流程同、多处不同行为） | 是（`WorkerBase`） | — | — | — |
| 差异是**一个无状态算法**（同骨架、单点替换） | — | 是（`LowGainPolicy`） | — | — |
| 需要**延迟到别的线程/状态执行** | — | — | 是（`IrscCmd`） | — |
| **顺序本身是契约**、要以数据表审计 | — | — | 是（`kIrscCmdSequence`） | — |
| 需要向**固定集合**广播/传播（init/deinit） | — | — | — | 是（`SessionEventComposite`） |
| 需要 vtable 运行期多态 | 否——用 CRTP | 否——用 Policy | — | — |
| 需要"is-a"类层次继承 | 否——组合 + 钩子代替 | — | — | — |

两条总序：**静态多态（CRTP/Policy）优先于运行时多态（vtable）；组合优先于继承**——继承只出现在 CRTP 的"骨架 + 钩子"形态里，且基类不持有派生专属状态。

无法对号入座时：先写两个直接的普通函数/struct，等第三个相似实例出现再回到本表。

## 7. 风格与注释

- **Allman 大括号**、4 空格缩进、**120 列**。
- 英文注释、`/* */` 风格（行尾短注释可用 `//`，现有代码以 `//` 分节横线为主，保持一致即可）。
- 文件头：模块一句话定位 + 与真实系统的映射表（若为示例）+ `SPDX-License-Identifier: MIT`。
  - 落点：isp_pipeline_demo.cpp 头部 "Mapping to RS500" 表。
- **决策注释义务**：反直觉的选择（不加锁、丢弃语义、单槽深度、黑板禁令、entry 而非 action 发硬件命令）必须在代码处写明"为什么"，且注释要能被下一个人单独读懂。
- 命名：
  - 类型/函数 `PascalCase`；变量/字段 `snake_case`；常量/枚举值 `k` 前缀（`kFrameCount`、`Sig::kBoot`、`kDdrDn`）；
  - 信号枚举 `Sig::kXxx`；事件数 `LogEvt` 与 `Sig` 分开命名；
  - guard 函数名回答是/否问题（`irsc_session_open`、`cmd_is_pic`、`at_quiesce`）；action 函数 `onXxx` / `rcEnterXxx` 前缀分层。
- **RAII 装饰器**：成对操作（进入/退出必须同时发生）包成 guard 对象，拷贝/赋值 `= delete`；显式 `start/stop`、`init/deinit` 生命周期用于跨事件边界的长寿命资源（配对语义写头注释，见 3.3 drain-on-stop）。
  - 落点：`BypassGuard`（进入置 `cache_bypass`，任意退出路径 resync——"an early return cannot skip the resync"）；框架层 `CriticalSection::Token`（queue.hpp，临界区进入即取 token、作用域结束即释放）。

### 7.1 错误处理：`Expected` 与错误码

- **禁用异常**（`RT_ASSERT` 同禁，断言只用于框架内部不变量）。错误用值语义返回：
  - 简单场景：bool / 错误码枚举（`enum class InitError : uint8_t`，pal_rtthread.hpp / config.hpp）；
  - 值或错误二选一：`coact::Expected<V, E>`（include/coact/expected.hpp）——`[[nodiscard]]` 整类标注、`success(V&&)/error(E)` 工厂、`Expected<void,E>` 特化、支持 move-only `V`、固定内联存储（no exceptions, no heap）。
  - 落点：`AddrCache::query(uint32_t&)` 返回 bool（出参携带值——旧 API 风格，新代码优先 Expected）；`magx_supported` 返回 bool；`QueueResult` 融合结果（config.hpp，push 成败 + 队列水位一次返回，免二次进临界区）。
- 错误路径不得静默：返回值被消费或被计数（reject 弧计数器、`rejected_count()`），无"丢弃返回值且无注释"的调用点。

### 7.2 平台适配层注意（PAL 边界收口）

以下条目**只适用于** `pal_rtthread.hpp` 及直接对接 RT-Thread C API 的适配代码，不进入业务/框架其余部分：

- `rt_kprintf` 仅允许 `%d %u %x %s %lu`（禁 `%llu/%zu/%f`），`size_t` 显式转 `(unsigned long)`。
  - 落点：`examples/log_rtthread_demo.cpp`；`include/coact/diag/log_rtthread.hpp` 适配通道。
- 文件 I/O 一律 POSIX `open/read/write/close`（禁 `fopen/fread/fwrite`）；格式化用 `snprintf`（禁 `sprintf`）。
- 适配层若使用 `rt_malloc`，必须配对 `rt_free` 且释放后置 `nullptr`；但框架/示例的业务路径零堆，不存在此调用。
- host 侧示例可用 `std::printf`；面向 RT-Thread 打印通道时按上一条约束。

### 7.3 其他

- 自验证：示例程序结尾必须断言全部不变量并以退出码给出结论（ctest 可门控）。
  - 落点：isp_pipeline_demo.cpp 末尾 verification 块 + `RESULT: ALL PASS`。

## 8. 检查清单（review checklist）

逐项打勾；任一"否"即 review 不通过。

### 总则

1. [ ] 仅使用 C++17 特性，未引入 C++20 语法/库？
2. [ ] 无 `#ifdef` 平台分支（平台差异全部经 PAL / Profile 模板参数）？
3. [ ] 业务代码零堆分配（无 `new`/`malloc`），存储为静态/栈上/编译期容量容器？
4. [ ] 代码与注释为英文，无中文混入？
5. [ ] rt_kprintf/POSIX I/O 等 C 接口只出现在 PAL/演示打印层，未渗入业务代码（7.2 收口）？

### 类型与内存

6. [ ] 无裸 `int/long/char`；全部 `<cstdint>` 固定宽度整型？
7. [ ] 宽度/符号转换处均有显式 `static_cast`？
8. [ ] 比较表达式常量在左（`0 == x`、`nullptr == p`）？
9. [ ] 新枚举均为 `enum class` + 底层类型；无 `#define` 常量、无裸 `enum`？
10. [ ] 单语句分支也带 `{}`；全文件无 `goto`、无递归？
11. [ ] `switch` 有 `default` 或穷举 + 兜底返回？
12. [ ] 无动态分配池/工厂模式；行为多态走模板（CRTP/Policy），确需擦除时用 const 函数表并说明理由？
13. [ ] 跨边界结构体在定义处有 `is_standard_layout`/`is_trivially_copyable`/`is_nothrow_move_constructible` 断言？
14. [ ] placement new 仅用于平凡可析构类型，读侧经 `std::launder`？
15. [ ] 原始未构造存储为 `std::byte` 数组 + `alignas(alignof(T))`，非 `char`/`uint8_t` 双关？
16. [ ] 跨边界连接用值语义 id（`TargetId` 等）或引用，非裸指针 + 所有权注释？
17. [ ] 成对操作已包成 RAII guard（拷贝 `= delete`），无裸 lock/unlock 配对调用？

### 线程与并发

18. [ ] AO 间仅事件通信，无共享可变状态跨越 AO 边界？
19. [ ] 数据平面字节留在 DDR/owner，事件只带描述符（零拷贝）？
20. [ ] 锁层级 L1→L2→L3，无反向获取？
21. [ ] 每处"不加锁"的决定都有注释论证（最弱足够原则）？
22. [ ] 跨线程标志/枚举为 `std::atomic` 且断言 `is_always_lock_free`？
23. [ ] worker 交接为单槽/浅环、忙则拒绝 + 计数，无阻塞排队？
24. [ ] `stop()` 语义（drain vs 丢弃）已声明且被注释论证？
25. [ ] 关键区内无硬件延迟/长操作？

### 函数与控制流

26. [ ] 每个函数 return 数 ≤ 5？
27. [ ] guard 均为纯函数（只读、`noexcept`、无副作用）？
28. [ ] 硬件命令在 entry、清理在 exit、事件响应在 action，未错层？
29. [ ] 状态机为静态表驱动，非法 (状态, 事件) 有显式 reject 弧？
30. [ ] 事件块只 submit 不手动回收；程序结束时 `pool.used() == 0` 可验证零泄漏？
31. [ ] AO 属性走 Trait 结构体；运行期观测走 monitor，无散装打印探针？
32. [ ] 排空逻辑用 `pending()`/终态条件，无固定 sleep 赌时序？

### C++17 特性

33. [ ] `if constexpr` 只用于"未选分支不该被实例化"的场景？
34. [ ] 常量表为 `constexpr`/`inline constexpr`，且非为 constexpr 而 constexpr？
35. [ ] `[[nodiscard]]`/`noexcept`/`explicit` 按语义使用，未机械全标？
36. [ ] `std::exchange` 仅用于"取旧+置新"一体交接；弃返回值处写 `static_cast<void>`？
37. [ ] 跨槽位/跨线程交接用 `T&&` + `std::move`；失败路径不消费调用者的值；move 后源不再读？

### 设计模式

38. [ ] 每个模式（CRTP/策略/命令/组合）的引入满足第 6 章准入条件，可指出三个相似实例或等价证据？
39. [ ] CRTP 钩子面 ≤ 7 个且基类未滥用状态？
40. [ ] 策略无状态、方法 `noexcept`？
41. [ ] 命令对象自包含；顺序契约以数据表固化而非 if 链？
42. [ ] 组合为编译期固定集合 + 展平循环，无递归？
43. [ ] 静态多态优先于 vtable；组合优先于继承，未引入非必要类层次？

### 风格

44. [ ] Allman / 4 空格 / 120 列；命名符合第 7 章前缀约定？
45. [ ] 反直觉决策处均有"为什么"注释？
46. [ ] 错误路径有消费或计数（Expected/错误码被处理），无静默丢弃返回值？
47. [ ] （示例程序）结尾自验证不变量并以退出码给出结论？

## 附：规约与代码落点速查

| 规约条目 | 代码落点 | 一行说明 |
|---|---|---|
| CRTP 骨架+钩子 | `WorkerBase` (isp_pipeline_demo.cpp) | 互斥环+drain+计数骨架，派生只写 `execute` |
| CRTP 静态钩子 | `VideoFsmNode<PicFsmNode>` | 双流 FSM 共享骨架，差异全在钩子 |
| 策略参数化 | `GainNodeBase<LowGainPolicy>` 等 | 无状态 `apply()` 定制点，编译期分发 |
| 策略门面 + if constexpr | `QuiescePolicy<HasIdleIrq>` | 中断/轮询两实现，未选者不实例化 |
| 命令顺序契约 | `kIrscCmdSequence` + `IrscCmd::stamp` | init→start→ctrl→output_enable 即合同 |
| 命令完成姿态表 | `kIrscDoneTable` + `irsc_done_action_of` | 表驱动取代 if 链 |
| 组合展平广播 | `SessionEventComposite::publish` | 固定数组迭代，零递归 |
| std::exchange（14 处） | `UsbDmaWorker::run`、`complete_irq`、`rcEnterCommit` 等 | 槽位取走重置/状态交接一体 |
| placement new + launder | `DdrCtx::write/read`、`WorkerBase::submit/run`、`AddrCache::store` | DDR 帧戳/worker 槽/缓存槽就地构造（零拷贝） |
| std::byte + alignas 原始存储 | `SlotStorage<T>`（queue.hpp）、`Expected::Storage`（expected.hpp）、`EventBlockLayout`（pool.hpp） | 未构造存储的标准正字标记 |
| move 语义所有权 | `BoundedMpscQueue::try_push_observed(T&&)`（queue.hpp） | 容量确认后才 move，失败不消费调用者值 |
| Expected 值或错误 | `coact::Expected<V,E>` / `Expected<void,E>`（expected.hpp） | [[nodiscard]]、move-only V、固定内联存储零异常零堆 |
| RAII 装饰器 | `BypassGuard`、`CriticalSection::Token` | 任意退出路径自动配对 |
| tagged-CAS 无锁池 | `coact::EventPool` + `HostSmpProfile` (pool.hpp) | 32 位 tagged 原子头 + CAS，示例经 `alloc_typed` 免锁取块 |
| ABI static_assert 块 | `FrameStamp`/`IoMeta`/`VideoCtx` 等定义处 | 违约编译失败而非现场崩溃 |
| lock-free atomic 断言 | `g_session` 等 `is_always_lock_free` | 杜绝 libatomic 隐藏锁 |
| 单写者 + 事件传播（非黑板） | `g_zoom`/`g_sel`/`g_wrape` 注释块 | 物理寄存器建模，禁黑板条件写明 |
| 最弱足够同步 | `DdrCtx` 定义注释 | Dispatcher 单线程序列化即互斥，不加锁 |
| entry 发硬件命令 | `rcQuiescingEntry`/`rcResumeEntry` | transition action 里自提交会竞态拓扑 |
| guard 纯函数 | `irsc_session_open`/`at_quiesce` 等 | 只读+noexcept，副作用在 action |
| reject 弧显式化 | `kVideoTransitions` 尾部 + `onVideoCmdRejected` | 非法事件计数+trace，不静默丢 |
| 零堆事件池存储 | main 中 `alignas(kPayloadAlign) storage` | 调用方供存储，池零堆依赖 |
| PAL 边界收口（C 接口） | `pal_rtthread.hpp`、`log_rtthread.hpp`、log_rtthread_demo.cpp | rt_kprintf/POSIX I/O 只在此层 |
| 事件引用计数零泄漏 | `Event::ref_ctr`（event.hpp）+ verification 块 `pool.used() == 0U` | 框架管理回收，业务只 submit |
| AO Trait 静态属性 | `AoTrait<Prio>` / `VideoFsmTrait` | 优先级/RTC 预算编译期定型 |
| monitor 可观测性 | `rtc_timeouts` / `disposition_overload` / `pending()`（monitor.hpp） | 计数器观测，不加打印探针 |
| TransitionKind 语义 | `TransitionKind`（hsm.hpp）Internal/Self/External | guard 决定种类，action 不感知 |
| 事件驱动排空 | main 两处 drain 循环（`pending().load()` 归零） | 禁固定 sleep 赌时序 |
| 主线程发起会话广播 | `request_recfg` lambda 注释 | Dispatcher 上下文自提交会丢唤醒 |
| 自验证退出码 | isp_pipeline_demo.cpp verification 块 | ctest 可门控的 PASS/FAIL |

（完）
