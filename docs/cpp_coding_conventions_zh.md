# coact C++ 编码规约（中文版）

本文档是 coact 仓库 C++ 代码的完整编码规约，视角是**现代 C++17**——不是从 C/MISRA 移植过来的 C-with-classes，而是编译期确定、零拷贝、低分支、静态多态优先的 C++17 表达。目标读者：在本仓库编写或评审 C++ 代码的工程师与自动化审查代理。每条规约均写成可判定形式——评审时对每条回答"是/否"即可，不存在模糊表述。

规约锚点取框架层 `include/coact/`（`ao.hpp`、`hsm.hpp`、`pool.hpp`、`queue.hpp`、`spsc_ring.hpp`、`vocabulary.hpp`、`expected.hpp`、`coordinator.hpp`、`config.hpp`、`static_ao.hpp`、`pal.hpp` 等）——框架头是本仓库 C++17 规约的稳定实现，示例与测试代码按同一规约编写。`vocabulary.hpp` 是 `Expected` 的实现入口，`expected.hpp` 保留为兼容转发头。落点标注到文件与类名/函数名，不标注行号（行号随重构漂移）；抽象规约配以最小通用片段自证。

适用范围：仓库内全部 C++ 代码（框架头文件、示例、测试）。PAL 适配层的 C 接口约束单独收口在 7.2 节，正文不展开。

红线速查（详见各章）：禁止异常；禁止 `new/delete` 运行期堆分配（业务代码零堆）；禁止裸 `int/long/char`；禁止裸 `enum` 与 `#define` 常量；禁止裸指针代替引用/句柄（原始内存槽位除外，且须 launder 护住）；禁止动态分配池 / 工厂模式 / 过度抽象层；禁止跨 AO 共享可变状态；禁止 Dispatcher 上下文自提交会话级广播；禁止非平凡类型 placement new 进复用内存；禁止固定 sleep 排空；禁止为用而用任何 C++17 特性；三个相似才抽基类；组合优先于继承，静态多态优先于运行时多态。

## 1. 总则

### 1.1 语言与标准

- **C++17**。禁止使用 C++20 特性（concepts、ranges、`<format>` 等）。
- 可用的 C++17 核心能力以第 5 章的使用规约为准；语言特性本身允许不等于任何场景都该用。

### 1.2 双平台约束

- 所有 C++ 代码必须同时可在 **RT-Thread 目标机** 与 **Linux host** 编译运行。平台差异只允许通过 `include/coact/pal.hpp` / `pal_posix.hpp` / `pal_rtthread.hpp` 的 PAL 接口隔离，禁止在业务代码里出现 `#ifdef` 平台分支。
  - 落点：`coact::Runtime<Config, PalT, Profile = HostSmpProfile>` 以模板参数注入平台与并发语义（runtime.hpp），故池结构默认取 host SMP Profile；host 侧 `pal_posix.hpp`、目标机侧 `pal_rtthread.hpp`。
- 池等共享结构通过 **Profile 模板参数** 区分单核/SMP 语义，而不是 if/else：
  - 落点：`coact::RttSingleCoreProfile`（irq-mask 临界区，无 CAS）与 `coact::HostSmpProfile`（32 位 tagged 原子头 + CAS），见 `include/coact/pool.hpp` 头部注释；Profile 合法性由 `EventPool` 内 `static_assert` 把关。

### 1.3 现代 C++17 基调

本仓库的 C++ 有四条基调，全文档各章都是它们的具体化：

- **编译期确定、运行期少分配**：能在编译期定型的（表、策略、布局契约、AO 属性）一律 constexpr/模板/static_assert；运行期只留下"数据流动"本身，业务代码零堆分配。
- **零拷贝**：对象在目的地就地构造（placement new + 对齐存储），事件只传描述符不传数据字节；所有权移动用 `std::move` / `std::exchange`，不用复制。
- **低分支**：编译期路径分流（`if constexpr`）+ 表驱动（状态表、命令表）取代 if/else 链；热点循环内不引入可被 Profile 消除的分支。
- **边界清晰**：每个跨模块结构体在定义处用 `static_assert` 类型萃取声明契约；每个并发边界（AO 间、worker 交接）只经事件平面；违反任何边界 = 编译失败或显式 reject 弧，绝不静默。

### 1.4 语言与注释语言

- 代码、注释、commit message 用英文；面向人的文档（本文件）用中文。示例层中的中文注释为存量，新增代码不再扩展此风格。

## 2. 类型与内存安全

### 2.1 类型纪律（现代 C++ 表达）

- **固定宽度整型**：优先 `<cstdint>` 体系（`uint8_t / uint16_t / uint32_t / int8_t / int32_t`），禁裸 `int / long / char / unsigned`；并把"该类型宽度即契约"交给类型萃取把关（`sizeof`/`is_standard_layout` 断言），不靠人肉记忆。
- **强类型代替弱转换**：领域枚举用 `enum class X : 底层类型`（禁 `#define` 常量、禁裸 `enum`）；可判空的语义类型用 `explicit operator bool()` 而不是返回裸 int/指针；错误用 `Expected`/错误码枚举而不是裸 int 返回。
  - 落点：框架层 `enum class TransitionKind : uint8_t`（hsm.hpp）、`enum class PriorityClass : uint8_t`（config.hpp）、`enum class InitError : uint8_t`（config.hpp / pal_rtthread.hpp）；`Expected::explicit operator bool()`（vocabulary.hpp，`expected.hpp` 为兼容入口）。
- **隐式转换显式标注**：任何跨宽度/跨符号赋值必须写 `static_cast<目标类型>(...)`。
  - 落点：`static_cast<uint8_t>(sizeof(rt_tick_t) * 8U)`（pal_rtthread.hpp）等框架层一致使用。
- 常量左侧（Yoda 比较）：`0 == x` / `nullptr == p`。
  - 通用片段：`if (nullptr == p) { return; }`、`if (0U == count) { ... }`。

### 2.2 健壮性基础（对齐 MISRA C++ 视角）

本节把 MISRA C 时代的纪律直接落到 MISRA C++ 的视角上：MISRA C++ 用**规则分级**（Mandatory 强制 / Required 必需 / Advisory 建议）与**指令**（Directive，面向流程与非语言约束）表达要求，其核心手段是"用类型系统、初始化与析构在编译期消除整类缺陷"，而不是靠人肉记忆控制流。下列条目按 MISRA C++ 的对应主题归类：

- **复合语句纪律（compound statement）**：`if`/`for`/`while`/`do-while` 的分支体一律用 `{}` 包裹，即使单语句——对应 MISRA C++ 关于选择/迭代语句体必须为复合语句的规则，防止悬挂 else 与后续插入语句时的作用域漂移。
- **禁 `goto`（jump statement）**：MISRA C++ 禁用 `goto` 及跨越作用域的跳转；控制流清理改由析构保证、HSM 转移表和提前 return 承担——这既满足"不在中途跳出块"的规则，又把资源释放交还给 RAII。
- **禁递归**：MISRA C++ 将递归列为受限项（栈深不可静态界定）；组合/树遍历改用编译期固定深度或展平循环（见 6.4）。
- **`switch` 完整（well-formed switch）**：每个 `switch` 必须带 `default` 标签、每个 clause 有明确终止——对应 MISRA C++ 关于 switch 子句完整与不落入（fall-through）的规则；`enum class` 交给编译器做穷举性检查，`default` 作兜底而非吞掉遗漏。
  - 落点：`Hsm` 对 `TransitionKind` 的分派 switch 带 `default`（hsm.hpp）。
- **资源获取即初始化（RAII）取代手工配对**：MISRA C++ 的指令要求"每条获取路径都有配对的、任意退出路径均可达的释放"——在 C++ 中由析构自动满足。malloc/free、lock/unlock、进入/恢复等成对操作整体包成 guard 对象，任何退出（含提前 return）都自动配对；裸 `new`/`delete` 与裸配对调用被禁用，所有权交给对象生命周期。
  - 落点：`CriticalSectionGuard`（pal.hpp，构造取 `CriticalSection::Token`、析构 restore——临界区进出自动配对）。

### 2.3 内存策略：编译期确定、运行期零堆

- **禁止动态分配池/工厂模式**。需要"多形态行为"时用模板参数化（CRTP/Policy 静态多态）或 `constexpr` 数据表；确需运行期擦除的少量场景用 const 函数指针表，但首选模板。
  - 落点：`coact::StaticAoEntry` + `make_static_ao_entry`（static_ao.hpp）是全仓库唯一的函数指针擦除点，且注释声明"deliberately does not replace AoBase ... until target measurements prove a benefit"。
- **静态/栈上存储优先，业务代码零堆**：事件池存储由调用方提供对齐的静态数组，池只做管理；容器一律编译期容量（模板参数），不 `new`/`malloc`。
  - 落点：`EventPool`（pool.hpp）接受调用方提供的存储块；框架层 `BoundedMpscQueue<T, Capacity>` 的 `Capacity` 模板参数与 `SlotStorage`（queue.hpp）、`SpscRing<T, Capacity>`（spsc_ring.hpp）、`Expected<V,E>` 的 fixed inline storage（"no exceptions, no heap"，vocabulary.hpp）。
- **侵入式链表代替堆容器**：排队结构内嵌 `next` 索引/指针，不额外分配节点。
  - 落点：`EventPool` 空闲链以块内 `next` 索引串接（pool.hpp，tagged head 无锁回收）。
- **减少裸指针 → 引用/句柄**：所有权清晰的传参用引用（`const T&`），跨线程/跨边界的可空句柄用 `TargetId`/`pool_id` 等值语义 id，不用裸指针 + 注释描述谁拥有。`reinterpret_cast` 只允许出现在"原始内存槽位 ↔ 对象"的 launder 桥两侧（见 2.4）。
  - 落点：`submit_from_task(TargetId target, Event* e, ...)`（coordinator.hpp）以 `TargetId` 值寻址目标 AO。

### 2.4 placement new 与对象生命周期纪律

- **placement new 仅用于平凡可析构类型**，且必须由 `static_assert(std::is_trivially_destructible<...>)` 或 `is_trivially_copyable` 在定义处护住；读侧必须经 `std::launder` 重建指针-对象关系（C++17 对象模型要求）。
  - 落点（写侧）：`EventPool::alloc_typed` 的 `::new (block) Layout{}` / payload 构造（pool.hpp）；`BoundedMpscQueue::try_push_observed(T&&)` 的 `::new (slot) T(std::move(v))`（queue.hpp）。
  - 落点（读侧）：`SlotStorage::slot_ptr` 经 `std::launder(reinterpret_cast<T*>(...))` 访问（queue.hpp）；`Expected::Storage` / `FixedFunction` 内联缓冲同构（expected.hpp / vocabulary.hpp）。
- **零拷贝**：placement new 是"目的地构造"的唯一手段——对象生命周期直接开始在池块/队列槽/内联缓冲的内存上，无临时对象、无拷贝。配合 `std::launder` 读侧，构成 C++17 对象模型下的完整零拷贝通道。
- 跨模块边界或进入池/队列内存的每个结构体，必须在**定义处**用 `static_assert` 声明 ABI/布局契约：`is_standard_layout` + `is_trivially_copyable` + `is_nothrow_move_constructible`。违约编译失败而非现场崩溃。
- **`std::byte` + `alignas` 是原始内存的正字标记**：未构造的原始存储用 `std::byte` 数组（不是 `char`/`uint8_t`），并在存储类型上 `alignas(alignof(T))`；访问一律经 placement new + launder 桥，不经裸类型双关。
  - 落点：`SlotStorage<T>`（queue.hpp，`alignas(alignof(T)) std::byte bytes[sizeof(T)]` + launder 访问器）；`Expected<V,E>::Storage`（vocabulary.hpp，同构）；`EventBlockLayout` 的 `alignas(PayloadAlign) std::byte payload[PayloadBytes]`（pool.hpp）。
- **move 语义即所有权语言**：跨线程/跨窗口交接用 `T&&` + `std::move`（`BoundedMpscQueue::try_push_observed(T&&)`——payload 只在容量确认后 move 一次，"A failed push does NOT consume the caller's value"，queue.hpp）；"取旧+置新"一体语义用 `std::exchange`（见 5.6）。对象必须 `is_nothrow_move_constructible` 才允许进这些通道（`Expected` 对 `V` 的构造前置断言即此纪律，expected.hpp）。

### 2.5 栈开销控制

零堆的另一面是**栈成为唯一的运行期伸缩空间**，而目标机线程栈是编译期定容的稀缺资源。栈开销按"每一帧多大 × 最深几帧"两个维度控制，任何一处放大都必须能被静态界定。

- **定容线程栈是硬预算**：Dispatcher / worker 栈大小在配置处一次性定死，业务代码不得假设有富余栈。新增代码抬高任一调用路径的最深帧，即等价于抬高所有线程的常驻内存。
  - 落点：`Config::kDispatcherStackBytes = 4096U`（config.hpp），`Runtime::start()` 把它 push 给 PAL；RT-Thread 侧 `RtThreadResources<StackBytes, ..., WorkerStackBytes>` 的 `alignas(RT_ALIGN_SIZE) rt_uint8_t stack[StackBytes]`（pal_rtthread.hpp）——栈是静态数组，容量即契约。
- **大对象不入栈帧——放静态/调用方存储**：MB 级的池存储、帧缓冲、查找表用 `static` 存储期或调用方提供的对齐数组，绝不作为局部变量出现在函数栈上；确需局部的小容器用编译期容量（模板参数），不用运行期 `new`。
  - 落点：池存储由调用方提供、`EventPool` 只管理不持有（pool.hpp）；所有容器 `Capacity` 为模板参数（`BoundedMpscQueue`/`SpscRing`），实例化即定死字节数。
- **按引用传大结构，按值传描述符/标量**：`sizeof` 超过若干字节的结构体一律 `const T&` 入参，不在调用点做栈上临时拷贝；小 POD（`TargetId`/`pool_id`/枚举）按值传，避免多一层间接。
  - 落点：`submit_from_task(TargetId, Event*, ...)` 的 id 按值（coordinator.hpp）。
- **事件只带描述符不带数据**：跨 AO 的事件块携带缓冲描述符（几个字节），真实数据留在拥有者管理的存储——这既省池也省栈，处理函数栈帧里不会出现大缓冲。事件体字节布局由 `EventBlockLayout` 的定长 payload 约束（pool.hpp，见 2.4）。
- **禁递归与深调用链**：递归在定容栈上不可静态界定（同 2.2）；树/组合遍历展平为固定深度循环。热路径调用深度应可被人工核验到"最深一条链 × 单帧上限 ≤ 栈预算"。
- **小缓冲类型擦除留在调用者栈，不外溢堆**：需要可调用对象承载捕获时，用固定内联缓冲的 `FixedFunction<Sig, BufferSize>`（默认 `2 * sizeof(void*)`），超出缓冲即编译失败（`static_assert(sizeof(Decay) <= BufferSize)`）而非静默回落堆——把栈占用显式化、有界化（vocabulary.hpp）。`Expected`/`FixedString`/`FixedVector` 同族"fully inline, no allocation ever"。
- **不引入可被消除的栈临时**：能在目的地就地构造的（placement new，见 2.4）就不先造临时对象再拷贝进槽位；所有权移动用 `std::move`/`std::exchange`（见 2.4、5.6），不在栈上留双份。

## 3. 线程与并发

### 3.1 AO 事件平面（Active Object）

- AO 之间**只通过事件通信**：`pool.alloc_typed` 分配事件块 → `coordinator().submit_from_task(target, &e->event, ...)` 投递。禁止跨 AO 共享可变状态。
  - 落点：`EventPool::alloc_typed`（pool.hpp）+ `Coordinator::submit_from_task`（coordinator.hpp）。
- 非 AO 线程（worker、ISR 模拟）与 AO 的**唯一耦合是事件平面**；worker 不读 AO 内部字段。
- **数据平面（像素字节）不走事件**：事件只携带缓冲描述符，真实数据留在 DDR 由拥有者 AO 管理（承 2.5"事件只带描述符"）。
  - 落点：`submit_from_task` / `try_submit_from_isr` 只接管 `Event*` 描述符的所有权（coordinator.hpp 头注释）。

### 3.2 锁层级与最弱足够同步

- 锁层级 **L1 Singleton → L2 Context → L3 Device**，禁止反向获取。
- **最弱足够原则**：若数据已被互斥机制（单 Dispatcher 线程序列化、AO 队列、单写者）覆盖，**不加锁**，且必须写注释论证为什么不需要锁。
- 模拟硬件寄存器的全局允许存在，但必须**单写者**（一个 AO action 写）+ 同 Dispatcher 线程读 + 变更经事件传播，并写明它不是黑板。黑板（多生产者 + 轮询消费者）仅在多传感器融合类需求下允许。
- 跨线程枚举/标志用 `std::atomic` 且必须断言 `is_always_lock_free`（libatomic 回退是隐藏的锁/堆依赖，构建期必须失败）。
  - 落点：`EventPool` 用 `if constexpr` 按 Profile 分流——最优路径强制 `std::atomic<uint32_t>::is_always_lock_free` 断言（pool.hpp：非 Rtt 单核 Profile 即拒绝非 lock-free 原子）。

### 3.3 Worker 交接契约：单槽、忙则拒绝 + 计数

- **worker 单槽/浅环交接**：交接点满则返回失败，**不阻塞、不排队**；调用侧计数丢弃。丢帧是诚实的计数器，不是隐藏的停顿。
  - 落点：`BoundedMpscQueue::try_push_observed` 满则返回失败 `QueueResult`（不阻塞，queue.hpp）；`SpscRing` 的 `try_push` 同构（spsc_ring.hpp）。
- **drain-on-stop**：停机先排空在途任务再退出（每个被接受的 job 必须产出完成事件）；与"忙则丢弃"语义并存时**两者都要注释写明差异**。
- 关键区只含少量 store，**绝不把硬件延迟圈进锁内**；耗时操作在锁外执行。
  - 落点：临界区回调 `cs.save/cs.restore` 仅包几个 store（pal.hpp `make_critical_section` / `make_spin_critical_section`）。

## 4. 函数与控制流

### 4.1 return 预算

- **单个函数的 return 语句不超过 5 个**；能不提前 return 就不提前 return。错误路径可提前返回，但超过 5 个 return 说明函数职责过多，应拆分。

### 4.2 guard / entry / exit / action 分层

- **guard 是纯函数**：只读 ctx 与 event、无副作用、`noexcept`、返回 bool。副作用禁止出现在 guard 里。
- **entry 只做硬件命令**：进入状态时发出的硬件操作（寄存器写、启停命令）必须放在 entry action，**不得放在 transition action**（transition action 在状态切换前执行，从那里自提交事件会与拓扑竞争）。
- **exit 只做清理**：退出状态时的资源释放/计数收尾。框架由 `Hsm::exit_to_lca`（hsm.hpp）按状态栈逐层调用 `StateDef::exit`。
- **action 是事件响应**：transition action 只做"本弧的业务效果 + 更新镜像枚举 + 链接下一事件"，控制流归 HSM 拓扑。

### 4.3 状态机表驱动

- AO 行为一律**静态 HSM 表**驱动（`StateDef[]` + `TransitionDef[]`），禁止在 action 里 if-else 模拟状态机。
  - 落点：`Hsm<Context>` 以状态/转移表驱动（hsm.hpp）。
- 拒绝路径必须显式：非法 (状态, 事件) 对落到 Self 转移的 reject 弧（计数 + trace），禁止静默丢弃。
  - 落点：`TransitionKind::Self`（hsm.hpp）承载自环/拒收。

### 4.4 事件生命周期与运行期监控

- 事件块由池分配（`alloc_typed`）后，引用计数（`Event::ref_ctr`，event.hpp）由框架管理：alloc 后为 1，每多投递一次 +1，归 0 回收。业务代码**只投递（submit）不手动回收**；程序结束必须断言 `pool.used() == 0U`（零泄漏）。
  - 落点：`Event::ref_ctr`（event.hpp）、`EventPool::used()`（pool.hpp）。
- AO 静态属性（优先级、RTC 预算、直投资格）一律走 **Trait** 结构体，不通过构造参数或运行期 setter。
  - 落点：`coact::Ao<Context, HsmT, Traits>` 三参数形态，`Traits::logical_prio()` / `priority_class()` / `direct_eligible()` / `kRtcBudgetNs`（ao.hpp）。
- 运行期可观测性来自 monitor（`rtc_timeouts` / `disposition_overload` / `pending` 计数器），不往业务代码里加打印探针。
  - 落点：`include/coact/monitor.hpp` 的原子计数器族。
- HSM 转移种类用对：`TransitionKind::Internal`（只跑 action）、`Self`（拒收/自环）、`External`（离开子树再入目标，边界状态重跑 entry/exit）。
  - 落点：`TransitionKind` 三值定义及 `Hsm` 分派 switch（hsm.hpp）。
- 排空（drain）用事件驱动条件（每 AO `pending()` 归零 + FSM 回到终态），**禁止固定 sleep 赌时序**。
  - 落点：`pending` 高水位/当前计数（monitor.hpp）。
- 主线程提交会与 Dispatcher 竞争唤醒闩锁的场景（会话状态广播），必须从**主线程**发起而非 Dispatcher 上下文自提交，并注释论证。
- AO 上限由配置约束（`kMaxAo = 16`，config.hpp）；AO 合并（如两条流并入一个 HSM 的状态乘积）是达标手段，合并后状态命名编码各流相位。

## 5. C++17 特性使用规约

每特性三段式：何时用 / 红线 / 代码落点。

### 5.1 `if constexpr`

- **何时用**：同一模板需要按编译期条件实例化不同路径，且不需要的那条路径根本不该被实例化（依赖不存在、或想消除运行期分支）。
- **红线**：禁止用 `if constexpr` 包裹恒真/恒假条件来"预留未来分支"；运行期才知道的条件必须用普通 `if`。
- **落点**：`EventPool` 内 `if constexpr (detail::is_single_core_pool_profile_v<Profile>)`（pool.hpp）按 Profile 分流单核/SMP 实现；`pal_rtthread.hpp` 的 tick 换算按 `RT_TICK_PER_SECOND` 分流。

### 5.2 `constexpr` / `inline constexpr`

- **何时用**：模式表、命令表、调参常量、枚举名表——所有"数据即契约"的常量集合（`enum class` + `constexpr` 表共同取代 `#define` 常量）。头文件内表用 `inline constexpr` 避免 ODR。编译期确定的值一旦被 `static_assert` 或模板消费，即锁进二进制。
- **红线**：不为 constexpr 而 constexpr（运行期才确定的值就用普通变量）；禁止把大表写成 constexpr 但从未被编译期消费（如 static_assert 或模板）又声称收益。
- **落点**：`inline constexpr uint16_t kReclaimBatchCap`（pool.hpp）、`reclaimer_pool_capacity()`（constexpr 函数，pool.hpp）、`kMaxAo`（config.hpp）。

### 5.3 `enum class` + 底层类型

- **何时用**：一切新枚举；跨边界/进事件的枚举必须带底层类型。
- **红线**：禁止裸 `enum`（无作用域枚举）用于新代码。
- **落点**：`TransitionKind : uint8_t`（hsm.hpp）、`PriorityClass : uint8_t`、`InitError : uint8_t`（config.hpp）。

### 5.4 `static_assert` 类型萃取

- **何时用**：凡是"该类型必须满足 X"的假设，一律在定义处或模板内断言（`is_standard_layout` / `is_trivially_copyable` / `is_nothrow_move_constructible` / `atomic<T>::is_always_lock_free` / `is_same`）。违约必须编译失败，不许到现场才炸。
- **红线**：禁止断言显然为真的平凡事实凑数（如 `static_assert(sizeof(char) == 1)`）。
- **落点**：`EventPool` 内 Profile 合法性与 CAS lock-free 断言（pool.hpp）；`FixedFunction` 的 `static_assert(sizeof(Decay) <= BufferSize)`（vocabulary.hpp）。

### 5.5 `[[nodiscard]]` / `noexcept` / `explicit`

- **何时用**：
  - `[[nodiscard]]`：返回值承载错误/所有权/关键结果的函数（忽略即 bug）；整个错误类型可直接 `class [[nodiscard]] Expected final`；
  - `noexcept`：不抛函数（move、纯查询、guard、静态策略）——本仓库禁异常，`noexcept` 是接口契约而非优化提示；
  - `explicit`：单参构造与转换运算符（`explicit operator bool()` 让"有值"判断不会静默变 int）。
- **红线**：不整文件机械标注；`[[nodiscard]]` 用于"忽略它必然是错"的场景。
  - **落点**：`class [[nodiscard]] Expected final`（vocabulary.hpp，`expected.hpp` 为兼容入口）；`[[nodiscard]] QueueResult try_push_observed(T&&)`（queue.hpp）；`explicit operator bool()`（vocabulary.hpp）；`noexcept` 遍布 guard/静态钩子/policy。

### 5.6 `std::exchange`

- **何时用**：仅当需要**"取旧值 + 置新值"一体的原子语义交接**——即旧值确实被消费（移走、作为提交值），且置新值是交接的一部分。所有权跨窗口移动（ctx 槽位 → 出向事件）是典型场景。
- **红线**：单纯赋值不得硬改成 `std::exchange`；不消费返回值时写 `static_cast<void>(std::exchange(...))` 并保留注释，证明旧值曾被有意丢弃。
- 通用片段：
  ```cpp
  Job j = std::exchange(slot, Job{});   // 取走任务并重置槽位（消费旧值）
  static_cast<void>(std::exchange(slot, next));  // 覆盖旧值且有意丢弃
  ```

### 5.7 placement new + 对齐存储

- **何时用**：在原始内存（事件池块、队列槽、内联缓冲）中就地构造，见 2.4。对齐由 `alignas`/对齐常量在**存储声明处**保证，不在使用处补救。
- **红线**：非平凡可析构类型禁止 placement new 进复用内存；禁止 placement new 后不经 `std::launder` 直接 `reinterpret_cast` 读。
- **落点**：`EventBlockLayout<IoMeta, PayloadBytes, PayloadAlign>` 的 `alignas(PayloadAlign)`（pool.hpp）；`SlotStorage<T>`（queue.hpp）；其余见 2.4。

### 5.8 `std::move` / 右值引用（值类别即所有权）

- **何时用**：跨线程/跨槽位交接对象时以 `T&&` 参数 + `std::move` 表达"源从此失效"；失败路径不得消费调用者的值（先查容量再 move）。
- **红线**：move 后的源对象禁止再读（源即已交接）；禁止对 const 对象强行 `const_cast` 后 move；复制成本可忽略的标量/描述符不必 move（过度 move 与漏 move 同罪）。
  - **落点**：`BoundedMpscQueue::try_push_observed(T&&)` + `::new (slot) T(std::move(v))`（queue.hpp，容量确认后才 move）；`Expected` 的 move 构造/赋值（vocabulary.hpp，move-only payload 的正字标记——拷贝被 `= delete`）。

### 5.9 特性使用总红线

- **不为用而用**：任何特性必须能回答"不用它会怎样"。
- **三个相似才抽基类**：出现第三处结构相似时才提取（CRTP/Policy/宏），两处时容忍重复。
- **禁过度设计**：helper / util / 抽象层最小化；宁可局部直白，不要全局优雅。
- 三处以上相似的 HSM 表可用宏压缩（如 `..._HSM_STATES` / `..._HSM_TRANS`），但宏必须保持"表即数据"（只拼表项，不嵌控制流），用后 `#undef`。

## 6. 设计模式使用边界

四个允许的模式，各自有明确的准入条件与红线。通用红线：**每个模式引入前必须能指出"三个相似实例"或等价的复用证据**；临界情况（两个实例 + 已知第三个在路上）需注释说明。

### 6.1 CRTP（骨架 + 钩子）

- **准入**：一个骨架类承载固定生命周期/流程（启停、环、计数、阶段流），各派生类只提供少量钩子；需要编译期分发、拒绝 vtable。
- **红线**：钩子数量失控（>7 个）说明骨架在猜未来，退回普通函数组合；CRTP 基类不得持有 per-instance 状态（静态钩子风格时）。
- **落点**：`Hsm<Context>`（hsm.hpp）以 Context 为 CRTP 参数，在编译期绑定派生 ctx 的表与钩子。

### 6.2 策略（Policy，算法族参数化）

- **准入**：同一算法骨架 × 可替换的无状态算法，策略是**只有静态 `apply()`（或等价静态方法）的无状态 struct**——`std::allocator` / `std::hash` 的定制点风格。
- **红线**：策略禁止携带状态（有状态策略改用 CRTP 或独立类）；策略方法必须 `noexcept`（可 `constexpr`）。
- **落点**：`RttSingleCoreProfile` / `HostSmpProfile`（pool.hpp）作为 `EventPool` 的 Profile 策略——无状态类型，经 `if constexpr` 在编译期选定实现；`coact::policy.hpp` 提供策略定制点。

### 6.3 命令（延迟执行 / 顺序契约）

- **准入**：(a) 操作需要携带自身身份/参数延迟投递执行；或 (b) 操作序列的**顺序本身是契约**，必须以数据表形式固化可审。
- **红线**：命令对象必须自包含（自带 tag/参数，不依赖调用点上下文）；禁止把命令表当成变相 if 链（每个命令一个几乎相同的 handler）。
- **落点**：`Event` 携带 `signal`（`alloc_typed(uint16_t signal)`，pool.hpp）作为延迟投递命令的身份；顺序契约以 `constexpr` 数据表固化（见 5.2）。

### 6.4 组合（树形结构传播）

- **准入**：需要向固定成员集合传播同一操作（init/deinit/状态广播）。
- **红线**：**编译期固定成员、禁递归**——组合被展平为固定数组的普通循环（"no recursion; the tree has fixed levels"）；成员集合运行期不可变。
- **落点**：`AoRegistry`（ao.hpp）以 `std::array<AoBase*, kCapacity>` 固定容量存储，遍历广播 init/deinit——编译期定容、无递归。

### 6.5 模式选择决策表

| 场景特征 | 选 CRTP | 选策略 | 选命令 | 选组合 |
|---|---|---|---|---|
| 差异是**钩子方法集合**（流程同、多处不同行为） | 是 | — | — | — |
| 差异是**一个无状态算法**（同骨架、单点替换） | — | 是（`Profile`） | — | — |
| 需要**延迟到别的线程/状态执行** | — | — | 是（`Event` signal） | — |
| **顺序本身是契约**、要以数据表审计 | — | — | 是（`constexpr` 表） | — |
| 需要向**固定集合**广播/传播（init/deinit） | — | — | — | 是（`AoRegistry`） |
| 需要 vtable 运行期多态 | 否——用 CRTP | 否——用 Policy | — | — |
| 需要"is-a"类层次继承 | 否——组合 + 钩子代替 | — | — | — |

两条总序：**静态多态（CRTP/Policy）优先于运行时多态（vtable）；组合优先于继承**——继承只出现在 CRTP 的"骨架 + 钩子"形态里，且基类不持有派生专属状态。

无法对号入座时：先写两个直接的普通函数/struct，等第三个相似实例出现再回到本表。

## 7. 风格与注释

- **Allman 大括号**、4 空格缩进、**120 列**。
- 英文注释、`/* */` 风格（行尾短注释可用 `//`，现有代码以 `//` 分节横线为主，保持一致即可）。
- 文件头：模块一句话定位 + `SPDX-License-Identifier: MIT`。
- **决策注释义务**：反直觉的选择（不加锁、丢弃语义、单槽深度、黑板禁令、entry 而非 action 发硬件命令）必须在代码处写明"为什么"，且注释要能被下一个人单独读懂。
- 命名：
  - 类型/函数 `PascalCase`；变量/字段 `snake_case`；常量/枚举值 `k` 前缀（`kMaxAo`、`kReclaimBatchCap`）；
  - guard 函数名回答是/否问题（`is_...`、`can_...`）；action 函数 `onXxx` 前缀分层。
- **RAII 装饰器**：成对操作（进入/退出必须同时发生）包成 guard 对象，拷贝/赋值 `= delete`；显式 `start/stop`、`init/deinit` 生命周期用于跨事件边界的长寿命资源（配对语义写头注释，见 3.3 drain-on-stop）。
  - 落点：`CriticalSectionGuard`（pal.hpp）；`= delete` 拷贝见 `Expected`（vocabulary.hpp）。

### 7.1 错误处理：`Expected` 与错误码

- **禁用异常**（`RT_ASSERT` 同禁，断言只用于框架内部不变量）。错误用值语义返回：
  - 简单场景：bool / 错误码枚举（`enum class InitError : uint8_t`，config.hpp / pal_rtthread.hpp）；
  - 值或错误二选一：`coact::Expected<V, E>`（include/coact/vocabulary.hpp；`expected.hpp` 为兼容入口）——`[[nodiscard]]` 整类标注、`success(V&&)/error(E)` 工厂、`Expected<void,E>` 特化、支持 move-only `V`、固定内联存储（no exceptions, no heap）。
  - 落点：`QueueResult` 融合结果（config.hpp，push 成败 + 队列水位一次返回，免二次进临界区）。
- 错误路径不得静默：返回值被消费或被计数（reject 弧计数器），无"丢弃返回值且无注释"的调用点。

### 7.2 平台适配层注意（PAL 边界收口）

以下条目**只适用于** `pal_rtthread.hpp` 及直接对接 RT-Thread C API 的适配代码，不进入业务/框架其余部分：

- `rt_kprintf` 仅允许 `%d %u %x %s %lu`（禁 `%llu/%zu/%f`），`size_t` 显式转 `(unsigned long)`。
  - 落点：`include/coact/diag/log_rtthread.hpp` 适配通道。
- 文件 I/O 一律 POSIX `open/read/write/close`（禁 `fopen/fread/fwrite`）；格式化用 `snprintf`（禁 `sprintf`）。
- 适配层若使用 `rt_malloc`，必须配对 `rt_free` 且释放后置 `nullptr`；但框架/示例的业务路径零堆，不存在此调用。
- host 侧示例可用 `std::printf`；面向 RT-Thread 打印通道时按上一条约束。

### 7.3 其他

- 自验证：示例程序结尾必须断言全部不变量并以退出码给出结论（ctest 可门控），例如 `RESULT: ALL PASS (fails=0)` / exit 0。

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
18. [ ] 新增/改动的调用路径已核算"最深帧 × 单帧上限 ≤ 线程栈预算"，无大对象入栈？

### 线程与并发

19. [ ] AO 间仅事件通信，无共享可变状态跨越 AO 边界？
20. [ ] 数据平面字节留在 owner，事件只带描述符（零拷贝）？
21. [ ] 锁层级 L1→L2→L3，无反向获取？
22. [ ] 每处"不加锁"的决定都有注释论证（最弱足够原则）？
23. [ ] 跨线程标志/枚举为 `std::atomic` 且断言 `is_always_lock_free`？
24. [ ] worker 交接为单槽/浅环、忙则拒绝 + 计数，无阻塞排队？
25. [ ] `stop()` 语义（drain vs 丢弃）已声明且被注释论证？
26. [ ] 关键区内无硬件延迟/长操作？

### 函数与控制流

27. [ ] 每个函数 return 数 ≤ 5？
28. [ ] guard 均为纯函数（只读、`noexcept`、无副作用）？
29. [ ] 硬件命令在 entry、清理在 exit、事件响应在 action，未错层？
30. [ ] 状态机为静态表驱动，非法 (状态, 事件) 有显式 reject 弧？
31. [ ] 事件块只 submit 不手动回收；程序结束时 `pool.used() == 0` 可验证零泄漏？
32. [ ] AO 属性走 Trait 结构体；运行期观测走 monitor，无散装打印探针？
33. [ ] 排空逻辑用 `pending()`/终态条件，无固定 sleep 赌时序？

### C++17 特性

34. [ ] `if constexpr` 只用于"未选分支不该被实例化"的场景？
35. [ ] 常量表为 `constexpr`/`inline constexpr`，且非为 constexpr 而 constexpr？
36. [ ] `[[nodiscard]]`/`noexcept`/`explicit` 按语义使用，未机械全标？
37. [ ] `std::exchange` 仅用于"取旧+置新"一体交接；弃返回值处写 `static_cast<void>`？
38. [ ] 跨槽位/跨线程交接用 `T&&` + `std::move`；失败路径不消费调用者的值；move 后源不再读？

### 设计模式

39. [ ] 每个模式（CRTP/策略/命令/组合）的引入满足第 6 章准入条件，可指出三个相似实例或等价证据？
40. [ ] CRTP 钩子面 ≤ 7 个且基类未滥用状态？
41. [ ] 策略无状态、方法 `noexcept`？
42. [ ] 命令对象自包含；顺序契约以数据表固化而非 if 链？
43. [ ] 组合为编译期固定集合 + 展平循环，无递归？
44. [ ] 静态多态优先于 vtable；组合优先于继承，未引入非必要类层次？

### 风格

45. [ ] Allman / 4 空格 / 120 列；命名符合第 7 章前缀约定？
46. [ ] 反直觉决策处均有"为什么"注释？
47. [ ] 错误路径有消费或计数（Expected/错误码被处理），无静默丢弃返回值？
48. [ ] （示例程序）结尾自验证不变量并以退出码给出结论？

（完）
