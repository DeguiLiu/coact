# 为什么嵌入式可以用 C++17

结论：在关闭异常/RTTI、禁止堆、静态分配的前提下，C++17 增加的是功能成本，
不是语言税。coact 在 Cortex-M4 + FreeRTOS 上的实测与等价 C 估算只差约 0.5–2 KiB。

## 实测成本

同一 FreeRTOS 内核、启动代码、链接脚本和 `-Os` 编译选项：

| 镜像 | Flash 增量 | RAM 增量 | 说明 |
|---|---:|---:|---|
| 裸 FreeRTOS 基线 | 0 | 0 | 调度器 + 1 任务 + 1 信号量 |
| coact 默认配置 | +8.9 KiB | +10.1 KiB | Dispatcher 栈 4 KiB、Staging 5.3 KiB |
| coact 小配置 + LTO | +8.0 KiB | +6.5 KiB | 容量裁剪后，仍通过 Renode 测试 |

等价 C 实现的估算为 +5–7 KiB Flash、+6–10 KiB RAM。差距来自模板实例化和通用
装配，约 1–2 KiB；RAM 主要由栈和队列决定，与语言无关。完整数据见
`docs/coact_freertos_footprint_zh.md`。

## 主要优势

- **编译期检查**：`static_assert`、`constexpr` 表、强类型句柄把容量、对齐、
  ABI 和状态机表错误挡在编译期，而不是靠运行时断言或代码评审。
- **零成本抽象**：模板静态多态替代虚表；`enum class`、`Span`、定容容器在
  优化后与手写 C 结构体同量级，不引入隐藏分配或调度。
- **确定性清理**：RAII 让临界区、预约回滚、句柄释放绑定对象生命周期，
  提前返回和错误分支不会漏释放。
- **类型安全**：`TargetId`、`LogicalPrio`、`Expected` 等类型阻止单位混用、
  参数顺序错误和静默失败。
- **同一工具链**：`arm-none-eabi-g++`、GDB、map 文件、Renode 调试方式不变；
  产物仍是普通 ELF，可继续用 `size`、`nm`、`objdump` 审计。
- **C++17 本身并不贵**：`constexpr` 表、`enum class`、`std::array`、`Span`、
  `string_view`、`static_assert` 都是零或近零运行时；贵的是异常、RTTI、
  `iostream`、`shared_ptr`、`std::function` 和热路径虚函数。

## 使用边界

- 必须关闭异常和 RTTI，禁止 `new/delete`、`iostream`、`std::function`、
  `shared_ptr` 等带堆或间接调用的设施。
- 热路径避免虚函数、深模板递归和 64 位软件除法；用 `-Os`、栈水位和
  `arm-none-eabi-size` 做门禁。
- 第三方库和 BSP 仍可用 C 接口；C++ 只负责模块边界和上层装配。

一句话：C++17 不是免费的，但成本可测、可控、可裁剪；换来的是更早发现错误、
更少的资源泄漏和更低的长期维护成本。
