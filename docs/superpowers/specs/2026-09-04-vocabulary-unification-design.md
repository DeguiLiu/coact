# Vocabulary 类型统一设计

## 目标

统一 `TargetId`、`TaskSlotId` 与 `coact::NewType` 的实现风格，并将当前被生产代码广泛使用的 `Expected<T, E>` 纳入 `vocabulary.hpp` 的统一入口；保留现有公共头文件和主要访问接口，避免无关的回调、容器和 PAL 类型改造。

## 现状与边界

- `NewType<T, Tag>` 已提供零开销强类型包装，但 `TargetId` 和 `TaskSlotId` 仍各自维护手写包装结构。
- `Expected<T, E>` 与 `vocabulary.hpp` 使用同一套固定内存、无异常设计，却独立定义在 `expected.hpp`。
- `ScopeGuard`、`FixedFunction`、`function_ref`、`FixedString`、`FixedVector` 暂无生产使用场景；函数指针、`std::array` 和 `CriticalSectionGuard` 保持不变。

## 设计

### 1. NewType 兼容能力

在 `NewType` 增加 `raw()`，返回与 `value()` 相同的底层值。这样现有 `TargetId` 的索引和监控代码无需大范围改名，`value()` 继续作为统一的新接口。

### 2. ID 类型统一

- `config.hpp` 定义 `TargetIdTag`，将 `TargetId` 改为 `NewType<uint8_t, TargetIdTag>`。
- `coro/task_id.hpp` 定义 `TaskSlotIdTag`，将 `TaskSlotId` 改为 `NewType<uint16_t, TaskSlotIdTag>`。
- 保留 `kInvalidTarget`、`kInvalidTaskId` 和显式构造语义。
- 将内部对 `TaskSlotId::value` 的访问改为 `raw()` 或 `value()`，不暴露存储成员。

### 3. Expected 统一入口

把 `Expected<T, E>` 及 `Expected<void, E>` 的现有实现移动到 `vocabulary.hpp`。`expected.hpp` 保留为兼容头，仅包含 `vocabulary.hpp`，因此既有 include 路径和 `coact::Expected` 名称不变，也不会产生重复定义。

### 4. 明确不改造项

- 不用 `FixedFunction` 替换现有静态函数表或函数指针。
- 不用 `FixedVector` 替换已有 `std::array`、Ring、Queue。
- 不把领域专用 `CriticalSectionGuard` 强行改成通用 `ScopeGuard`。
- 不修改错误枚举、结果结构和事件协议布局。

## 验证

- 词汇测试覆盖 `NewType::raw()`、`TargetId`、`TaskSlotId` 的零开销与强类型属性。
- 现有 `Expected` 测试继续通过，并增加从 `vocabulary.hpp` 直接使用 `Expected` 的编译/运行覆盖。
- 运行核心单元测试及项目既有构建命令，确认 ABI、事件协议和 coroutine 注册表行为无回归。
