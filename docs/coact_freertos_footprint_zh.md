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

## 7. 如果改用 C 实现（预估，非实测）

结论：等价功能的 C 实现大约增加 5–7 KiB Flash，RAM 与 coact 基本同量级
（6–10 KiB）。语言不是主要成本来源；要显著缩小体积，应当削减功能或容量。

| 档位 | 内容 | 预估 Flash 增量 | 预估 RAM 增量 |
|---|---|---:|---:|
| 等价 C 实现 | 单 Dispatcher、事件池、优先级队列、HSM 表、PAL 封装 | +5–7 KiB | +6–10 KiB |
| 极简 C 实现 | 1 个任务 + 1 个环形队列 + `switch` 状态机，无 AO 注册表/熔断器/监控 | +2–3.5 KiB | +4–5 KiB |

估算依据：

- coact 实测 Flash 增量为 9064 B。等价 C 版可省去模板实例化、通用装配和少量
  重复内联，预计省 30–40%，即落在 5–7 KiB。
- RAM 的主要项是 Dispatcher 栈（4 KiB）和 Staging（默认 5.3 KiB），C 版同样
  需要这些数据结构，预计只省 5–10%。
- coact 已使用 `-fno-exceptions -fno-rtti`，热路径没有虚函数，也没有堆；
  9 KiB 主要是实际逻辑，不是 C++ 语言开销。
- C 版的代价是失去 `static_assert`、RAII 回滚、强类型句柄和编译期容量检查，
  事件所有权与句柄配对只能靠约定和人工检查。

需要更准确的数字时，可以按“极简 C 实现”写一个参考镜像，复用
`test/renode/footprint.sh` 的同一套内核、启动代码和编译选项实测；上表不应作为
实测证据引用。

## 8. 优化当前 C++17 实现

结论：已经落地的两项优化（LTO、Config 裁剪）把增量从 +9.1 KiB Flash /
+10.4 KiB RAM 降到 **+8.2 KiB Flash / +6.5 KiB RAM**。其余代码级优化仍是估算，
没有计入这个结果。

### 8.1 已实测

`COACT_FOOTPRINT_LTO=1 test/renode/footprint.sh` 给 C++ 翻译单元加 `-flto`，
内核 C 文件保持非 LTO，避免 freestanding 链接丢掉 `memset`。

| 镜像 | text | data | bss | 相对 baseline text |
|---|---:|---:|---:|---:|
| coact（默认，LTO） | 15068 | 12 | 13504 | +8444 |
| coact_small（LTO） | 14780 | 12 | 9792 | +8156 |

| 措施 | 实测 Flash | 实测 RAM | 风险 |
|---|---:|---:|---|
| LTO | −620 B（默认）/ −144 B（小配置） | 0 | 低 |
| Config 容量降到 High 8 / Normal 16 / Low 32 | −764 B | −3712 B | 低 |
| 两项叠加 | **−908 B** | **−3712 B** | 低 |

优化后仍通过 Renode 运行：baseline、coact、coact_small 都打印 `PASS`，
Dispatcher 栈高水位保持 976/1024 字。

### 8.2 尚未实施的估算

| 措施 | 预估 Flash | 预估 RAM | 风险 |
|---|---:|---:|---|
| 去掉未使用的 Monitor / Breaker 计数 | −0.3–0.8 KiB | −0.1–0.3 KiB | 低，需要配置开关 |
| 静态 AO 入口替代虚表分派 | −0.2–0.4 KiB | 0 | 中，需改装配方式 |
| 消除剩余的 64 位除法 | −0.2–0.5 KiB | 0 | 中，需改时间域并验证回绕 |

依据与限制：

- 反汇编中仍有 `__udivmoddi4`（720 B），但至少一部分来自夹具的 64 位十进制
  输出（`put_u64`），不是 coact 运行时代码；只有产品镜像去掉全部 64 位除法后
  才可能拿到这部分收益，当前未实测。
- RAM 的下限主要由 Dispatcher 栈（约 4 KiB）和 Staging 决定。Dispatcher 栈实测
  高水位 976/1024 字，继续压缩需要先测量具体动作链，不能直接改小。
- `COACT_ENABLE_LTO=ON` 已加入 CMake，供产品构建复用同一优化；默认关闭，
  避免拖慢测试构建。

即使全部落地，C++17 版仍会保留静态断言、RAII 回滚、强类型句柄等收益；与 C 的
体积差距会缩小到约 0.5–2 KiB，而不是数量级差异。

### 8.3 模板实例化与通用装配还能省多少

- 单 Config、单 PAL 的产品镜像里，模板只实例化一次；跨翻译单元的重复已由 LTO
  合并（默认配置实测 −620 B）。单 TU 固件没有可再合并的重复实例。
- 多 TU 产品可用 `extern template` / 显式实例化收敛重复实例，预估 −0.2–0.6 KiB；
  这是一项构建结构调整，当前单 TU 镜像无法验证收益。
- 冷路径 `noinline` 需要逐函数判断。实测 `-fno-inline-functions-called-once`
  反而让 coact 增加 244 B，不能作为通用开关。
- `-fipa-icf` 与现有 `-Os` + LTO 的结果完全相同，说明相同代码折叠已经启用。
- 去掉未使用的 Monitor/Breaker 是剩下的主要代码项：预估 −0.3–0.8 KiB Flash，
  RAM 收益随 `kMaxAo` 增大。

C++17 本身并不贵：`constexpr` 表直接进 `.rodata`、无启动初始化；`enum class`
可指定 `uint8_t`；`std::array`、`Span`、`string_view` 只是指针加长度；
`static_assert`、`if constexpr` 零运行时；32 位 `std::atomic` 映射到
LDREX/STREX。真正昂贵的是异常、RTTI、`iostream`、`shared_ptr`、`std::function`
和热路径虚函数，这些在 coact 中都已禁用或避开。
