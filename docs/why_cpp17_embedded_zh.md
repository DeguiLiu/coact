# 为什么用 C++17，以及怎么用

结论：引入 C++17 不是为了追新，而是把容量、类型、生命周期、错误路径和模块边界
尽量前移到编译期与框架层，减少跨模块口头约定和联调缺陷。coact 的前提是关闭异常和
RTTI、禁止堆、静态分配，并保持 C/BSP 接口可连接。C++17 不替代 MPU、OS 保护域、
AUTOSAR 或功能安全认证流程。

## 为什么使用 C++17

### 成本可测

同一 FreeRTOS 内核、启动代码、链接脚本和 `-Os` 编译选项：

| 镜像 | Flash 增量 | RAM 增量 | 说明 |
|---|---:|---:|---|
| 裸 FreeRTOS 基线 | 0 | 0 | 调度器 + 1 任务 + 1 信号量 |
| coact 默认配置 | +8.8 KiB | +9.3 KiB | Dispatcher 栈 4 KiB、Staging 4.4 KiB |
| coact 小配置 + LTO | +7.9 KiB | +6.3 KiB | 容量裁剪后，仍通过 Renode 测试 |

等价 C 实现的估算为 +5–7 KiB Flash、+6–10 KiB RAM。按同一功能口径，coact
小配置相对等价 C 的差额为 Flash +1–2 KiB、RAM 0–1 KiB；这部分主要是模板实例化
和通用装配，不是 C++ 语言税。RAM 主要由栈和队列决定，与语言无关。完整数据见
`docs/coact_freertos_footprint_zh.md`。

### 安全与协作收益

- **安全约束前移**：`static_assert`、`constexpr` 表、强类型句柄把容量、对齐、
  ABI 和状态机表错误挡在编译期，而不是留到联调或运行时断言。
- **资源上限确定**：配置、队列和策略用模板静态多态；`enum class`、`Span`、
  定容容器不引入隐藏分配或调度，RAM 上限可在链接前核算。
- **模块边界清楚**：类和封装用于模块接口，组合优先于继承；只有 `AoBase` 保留
  一层受控虚调用。跨模块传递强类型，而不是裸整数和口头约定。
- **错误路径可审计**：RAII 让临界区、预约回滚、句柄释放绑定对象生命周期，
  提前返回和错误分支不会漏释放。
- **接口不易误用**：`TargetId`、`LogicalPrio`、`Expected` 等类型阻止单位混用、
  参数顺序错误和静默失败，减少集成阶段的隐性契约。
- **联调成本更低**：命名空间和函数重载用于隔离模块；底层 BSP 仍通过 C 接口连接，
  不需要为了使用 C++ 重写驱动。
- **受限的标准库与视图设施**：使用 `std::array`、`std::atomic`、`Span`、`string_view` 和
  冷路径算法；禁用分配容器、`iostream`、`std::function`、`shared_ptr`。
- **同一工具链**：`arm-none-eabi-g++`、GDB、map 文件、Renode 调试方式不变；
  产物仍是普通 ELF，可继续用 `size`、`nm`、`objdump` 审计。
- **C++17 本身并不贵**：`constexpr` 表、`enum class`、`std::array`、`Span`、
  `string_view`、`static_assert` 都是零或近零运行时；贵的是异常、RTTI、
  `iostream`、`shared_ptr`、`std::function` 和热路径虚函数。

### 模板在 coact 中的三种用法

- **函数模板与类模板**：`Hsm<Context>`、定容队列和 `Expected<V, E>` 把类型与容量
  放到编译期；存储后端使用静态数组或内存池节点，不使用动态分配。
- **模板特化**：用全特化或偏特化消除特定配置的分支，例如零槽快照环
  `AdmissionSnapshotRing<0U>` 不分配记录存储。硬件差异优先通过 Profile/PAL
  类型参数表达，避免散落的 `#ifdef`。
- **CRTP（奇异递归模板模式）**：`examples/isp_pipeline/sensor_irsc.hpp` 的
  `WorkerBase<Derived, ...>` 用编译期下转型绑定 `execute()`，不引入虚表。
  核心 HSM 没有强行套 CRTP，而是用模板参数绑定 `Context` 和函数指针表，
  达到同样的“骨架 + 钩子”效果。

## 使用边界

- 必须关闭异常和 RTTI，禁止 `new/delete`、`iostream`、`std::function`、
  `shared_ptr` 等带堆或间接调用的设施。
- 只实例化产品实际使用的 Config、PAL 和策略；多翻译单元用 LTO 或显式实例化
  收敛重复代码，新增模板实例前先比较 `size`。
- 热路径避免虚函数、深模板递归和 64 位软件除法；用 `-Os`、栈水位和
  `arm-none-eabi-size` 做门禁。
- `std::transform` 等标准算法不是禁用项；只在纯元素变换且边界清晰时使用。
  它不减少 RAM，热路径仍优先显式循环，以暴露临界区、早退和错误分支。
- 第三方库和 BSP 仍可用 C 接口；C++ 只负责模块边界和上层装配。

一句话：C++17 不是免费的，但成本可测、可控、可裁剪；换来的是更早发现错误、
更少的资源泄漏和更低的长期维护成本。

相关文档：`docs/cpp17_necessity_zh.md` 按"C 表达不出来 / C 能做但更贵 / C++ 无优势"
三类逐条论证必要性与边界，并给出采用门禁；`docs/coact_freertos_footprint_zh.md`
是体积与 RAM 的实测记录。
