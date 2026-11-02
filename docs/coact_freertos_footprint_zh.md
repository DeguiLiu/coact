# coact FreeRTOS 固件与 RAM 占用实测

结论：coact 不会引入堆、异常、RTTI 或隐藏的运行时，但它不是零成本。以本仓库的
默认配置运行一个 AO，Flash 增加约 8.9 KiB，静态 RAM 增加约 10.1 KiB；把
`Config` 容量调小后分别降到约 8.1 KiB 和 6.5 KiB。成本有界、可配置，是否可接受
取决于目标 MCU 的 Flash/RAM 预算，不能用“不会增大固件”来表述。

## 1. 测量方法

`test/renode/footprint.sh` 用同一套 FreeRTOS V11.1.0 内核、Cortex-M3 端口、启动
代码、链接脚本和编译选项构建三个 Cortex-M4 镜像：

| 镜像 | 内容 |
|---|---|
| `baseline` | 调度器 + 1 个静态任务 + 1 个计数信号量 + UART 输出，不含 coact 头文件 |
| `coact` | 相同启动任务和 UART；1 个 AO/HSM、默认 `DefaultConfig`、1 个 Dispatcher、8 块事件池 |
| `coact_small` | 同上，容量改为 High 8 / Normal 16 / Low 32，其余不变 |

编译选项：`-Os -ffunction-sections -fdata-sections`，链接使用 `--gc-sections`；
`FreeRTOSConfig.h` 中 `configSUPPORT_DYNAMIC_ALLOCATION=0`、
`configTOTAL_HEAP_SIZE=0`。因此表中数字不包含任何堆。

## 2. 实测结果（字节）

| 镜像 | text | data | bss | 合计 |
|---|---:|---:|---:|---:|
| baseline | 6624 | 8 | 3128 | 9760 |
| coact（默认） | 15688 | 12 | 13504 | 29204 |
| coact_small | 14924 | 12 | 9792 | 24728 |

| 相对 baseline | text | data | bss |
|---|---:|---:|---:|
| coact（默认） | +9064 | +4 | +10376 |
| coact_small | +8300 | +4 | +6664 |

## 3. 默认配置的 RAM 构成

| 项 | 字节 | 说明 |
|---|---:|---|
| Dispatcher 栈 | 4096 | `DefaultConfig::kDispatcherStackBytes`，可由板级 Config 覆盖 |
| Dispatcher TCB / 信号量 / ContextSlot | 316 | `g_res` 中 4096 字节之外的部分 |
| Staging 队列 | 5312 | High 32 + Normal 64 + Low 128 个槽位 |
| EventPool 存储 | 288 | 8 块 × 32 字节，调用方提供 |
| PAL 实例 | 80 | `FreeRtos` 对象本身 |
| AO 对象 | 56 | 一个 AO + HSM 表 |

合计约 10.1 KiB，与 `bss` 增量一致。`coact_small` 把 Staging 降到 1600 字节，
其余固定成本不变，RAM 增量为 6.5 KiB。

## 4. 栈水位

Renode 运行结果：

| 镜像 | 任务 | 高水位 | 栈容量 | 余量 |
|---|---|---:|---:|---:|
| baseline | boot | 478 字 | 512 字 | 34 字 |
| coact | boot | 478 字 | 512 字 | 34 字 |
| coact | dispatcher | 976 字 | 1024 字 | 48 字（192 字节） |

Dispatcher 在只有一层 HSM 动作时已用到 3.9 KiB。4 KiB 默认栈只剩约 4.7% 余量，
产品配置应按实际动作链加深测量后再定，建议预留 5–6 KiB。

## 5. 如何表述

可以说：

- coact 的 Flash/RAM 成本有界且可配置：默认约 +8.9 KiB Flash、+10.1 KiB RAM，
  小配置约 +8.1 KiB / +6.5 KiB。
- 没有堆、异常、RTTI、动态注册表或隐藏线程；线程、队列、事件池都是静态对象。
- 关闭准入策略时 `NoAdmission` 编译为空，快照环默认 0 槽，不占诊断存储。

不应说：

- “使用 coact 不会导致固件增大”。它会增加约 8–9 KiB 代码和 6–10 KiB RAM。
- “资源消耗可以忽略”。Dispatcher 栈和 Staging 是主要成本，必须按产品预算配置。

## 6. 复现

```bash
test/renode/footprint.sh
```

脚本输出三张镜像的 `arm-none-eabi-size`、最大 `.bss` 符号，并在 Renode 中运行
`baseline` 与两个 coact 镜像，打印栈高水位。
