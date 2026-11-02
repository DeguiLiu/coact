# MISRA C++ 适用范围与偏差记录（FreeRTOS PAL）

结论：新增的 `coact::pal::FreeRtos` 按 MISRA C++:2008 的可落地子集编写，生产代码不含动态分配、异常、RTTI、虚函数和宏实现的逻辑。本文不宣称整个 coact 仓库或第三方 FreeRTOS 内核已通过 MISRA C++ 认证；正式合规声明需要 MISRA 检查器与项目级偏差批准。

## 1. 适用范围

| 范围 | 文件 | 是否受 MISRA C++ 约束 |
|---|---|---|
| 生产 PAL | `include/coact/pal_freertos.hpp`、`src/core/pal_freertos.cpp` | 是 |
| 核心头文件 | `include/coact/pal.hpp`、`config.hpp`、`queue.hpp`、`pool.hpp` | 是，但本次只审计 FreeRTOS 端口引入的改动 |
| 第三方内核 | `test/renode/freertos/**` | 否，FreeRTOS 上游代码，原样引入 |
| host stub 与测试 | `test/freertos_stub.h`、`src/core/test_freertos_pal.cpp`、`test/renode/*_main.cpp` | 否，测试代码；生产镜像不链接 |

## 2. 生产代码已落实的规则

- 无 `new` / `delete` / `malloc` / `pvPortMalloc`，全部使用调用方提供的静态存储（MISRA C++ 18-4-1 方向）。
- 关闭异常与 RTTI，不用虚函数和动态多态；平台差异由模板参数在编译期解析。
- 所有句柄和资源数组在构造时值初始化，不使用未初始化对象。
- 类型转换使用 `static_cast` / `reinterpret_cast` 的明确形式，不使用 C 风格转换。
- 只读参数与局部量尽量 `const`；没有隐式窄化到更窄类型后再参与运算。
- 函数返回状态用 `pal::InitError` 枚举，调用方可判定失败，不静默降级。
- 无 `goto`、无递归、无可变参数、无多重声明、无八进制常量。
- 关键不变量用 `static_assert` 固化（空策略零存储、原子宽度、栈下限、优先级下限）。

## 3. 偏差

| 编号 | 规则（MISRA C++:2008） | 位置 | 理由与边界 |
|---|---|---|---|
| D1 | 5-2-8：整数不得转换为指针 | `test/renode/*_main.cpp` 的 MMIO 访问 | 裸机寄存器访问没有标准 C++ 表达方式；仅在 Renode 夹具中使用，生产 PAL 不含整数到指针转换 |
| D2 | 0-1-7：表达式值不得丢弃 | `src/core/pal_freertos.cpp` 中的 `(void)` 调用 | FreeRTOS 的 `xSemaphoreGive/Take` 返回值在不影响安全性的路径上故意忽略（例如唤醒信号量已满）；使用显式 `(void)` 标明意图，不丢弃有副作用的未使用结果 |
| D3 | 6-5-? / 控制流 | dispatcher/worker 的 `for (;;)` | FreeRTOS 任务函数不得返回；循环是任务终止条件，不是不可达代码 |
| D4 | 5-0-* 整数转换 | `static_cast<TickType_t>`、`static_cast<UBaseType_t>` | 目标 ABI 上宽度相同或已显式检查范围；`ms_to_ticks` 先做 64 位运算再收敛到 `TickType_t` |
| D5 | 第三方代码 | `test/renode/freertos/**` | FreeRTOS 内核与端口未按 MISRA 修改；只作为验证夹具，不进入产品源码统计 |
| D6 | 动态分配/线程 | `test/freertos_stub.h`、host 测试 | 仅 host 测试使用 `pthread`、`new`、`std::thread`；生产镜像不链接 |

## 4. 验证方式

当前环境没有 MISRA C++ 检查器。可复现的替代检查：

```bash
clang-tidy src/core/pal_freertos.cpp \
  --checks='-*,autosar-*,cert-*' \
  -- -std=c++17 -DCOACT_FREERTOS_STUB -Iinclude -I. -Itest
```

实测：0 条 AUTOSAR / CERT 告警；`-Wall -Wextra -Wpedantic -Wshadow` 编译无告警。

正式合规流程需要：

1. 固定 MISRA C++ 版本（2008 或 2023）与检查器配置；
2. 把本文偏差表转成检查器的 deviation permit；
3. 对生产编译单元（不含测试与第三方内核）跑全量规则并归档报告。

在此之前，本文只声明“按 MISRA C++ 子集编写并记录偏差”，不声明“MISRA C++ 认证通过”。
