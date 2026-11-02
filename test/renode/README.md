# coact Renode 验证夹具

结论：2026-11-02 实测，四个夹具全部通过，且每个夹具的判定都来自目标程序自己打印的结果行，不来自脚本超时。

| 夹具 | 命令 | 实测结果 |
|------|------|----------|
| 主夹具（准入 + 过载 + 事件池） | `test/renode/run.sh` | `RESULT PASS 231/231` |
| 基线管线（不要求准入 API） | `test/renode/run.sh --baseline-only` | `RESULT PASS 15/15` |
| 看门狗终止跑飞 | `test/renode/wdg.sh` | `RESULT WDG PASS 7/7`，实测 31899 us 复位 |
| MPU 能力矩阵 | `test/renode/mpu.sh` | `RESULT MPU PASS 13/13`，能力 11/11，设计要求项 8/8 |
| FreeRTOS PAL（真实内核） | `test/renode/freertos.sh` | `RESULT PASS 32/32` |
| FreeRTOS 故障路径 | `test/renode/freertos_wdg.sh` | `RESULT PASS 8/8`，关中断后 29.862 ms 复位 |
| 变异测试 | `test/renode/mutate.sh` | `ALL MUTATIONS CAUGHT`（13/13） |

镜像尺寸（`arm-none-eabi-size`）：主夹具 `text=36644 bss=716`；仅基线
`text=8276 bss=84`；看门狗 `text=5860 bss=104`；MPU `text=4744 bss=4116`。
FreeRTOS 夹具 `text=23100 bss=64076`（含并发 create/join 用例的 2 个 worker 池）；
FreeRTOS 故障夹具 `text=15548 bss=44820`。
准入策略关闭时协调器为 32 字节，开启后 120 字节（差值为 88 字节的策略
对象），ISR 版本同为 120 字节（`Cfg::kMaxAo=2`）。策略对象由 12 字节的
临界区钩子三元组、4 字节对齐填充和 3 个 24 字节的 `ArrivalGate` 组成。
快照环按配置计价：同一容量下 `Monitor<Cfg>`（8 槽）544 字节，
`Monitor<NoSnapCfg>`（不启用）280 字节。

## 1. 依赖

| 工具 | 实测版本 | 用途 |
|------|----------|------|
| Renode | 1.16.1 | Cortex-M4 功能模拟、串口文件后端、IWDG、MPU |
| arm-none-eabi-gcc/g++ | 10.3.1 | 交叉编译，`-std=c++17`，`-Os` |
| flock、timeout | util-linux/coreutils | 构建目录串行化与挂死保护 |

平台描述使用 Renode 自带的 `@platforms/boards/stm32f4_discovery.repl`；链接
脚本按 FLASH 128 KB、RAM 192 KB 布局，未触及模型上限。`timeout` 只用于防止
脚本挂死，任何情况下都不作为“有界终止”的证据。

## 2. 运行

脚本自行解析仓库根目录，可从任意当前目录执行：

```bash
test/renode/run.sh                 # 基线 + S1..S14
test/renode/run.sh --baseline-only # 仅基线管线
test/renode/wdg.sh                 # 看门狗
test/renode/mpu.sh                 # MPU 能力矩阵
test/renode/freertos.sh            # FreeRTOS PAL（真实内核）
test/renode/freertos_wdg.sh        # FreeRTOS 看门狗/故障路径
test/renode/mutate.sh              # 变异测试（内部多次调用 run.sh）
```

产物写入 `test/renode/build/`：各 ELF、`uart.log`（主夹具目标输出）、
`wdg.log`、`mpu.log`、`*_renode.log`（Renode 自身日志）。
`freertos.sh` 使用独立的 `build-freertos/`，避免与主夹具的 ELF 互相覆盖。
四个脚本共用 `build/`，各自持有独立的 `flock`，可安全串行调用。

## 3. 输出通道

本 Renode 版本不支持 Cortex-M 半主机：`machine EnableSemihosting` 返回
`machine does not provide a field, method or property EnableSemihosting`，
在平台中加入 `uartSemihosting: UART.SemihostingUart @ cpu` 也不会拦截
`BKPT 0xAB`。因此使用 USART2（`0x40004400`）+
`sysbus.usart2 CreateFileBackend` 输出到日志文件；目标端只轮询 SR.TXE，
不依赖中断。`.resc` 中的路径相对仓库根目录，脚本在启动 Renode 前切换目录。

`wdg` 夹具的 `.resc` 必须定义 `macro reset { sysbus LoadELF ... }`：否则
Renode 记录 `No action for reset` 并把 CPU 停在复位状态，第二次启动永远不会
发生。

## 4. 主夹具场景

目标程序为 `main.cpp`。虚拟时钟由 `harness::set_time_ns()` /
`advance_time_ns()` 显式推进，没有任何 sleep；S8 是唯一使用真实中断的场景。
每个断言输出一行 `PASS <name>` 或 `FAIL <name>`，最后输出
`RESULT PASS n/m`。

| 组 | 断言数 | 内容 | 证明了什么 |
|----|--------|------|------------|
| `baseline` | 15 | Direct / Staged / Policy 三条既有路径 | 基线管线在该 ABI 上可达，HSM 动作执行一次，策略拒绝返回原因码 |
| `S1` | 7 | `min_arrival_ns=1000` | t=0 准入、t=999 拒绝（reason=4）、t=1000 边界准入 |
| `S2` | 20 | 拒绝不消耗窗口 | 类契约与协调器端到端两侧都验证 `last_admit_ns()` 在拒绝后不变；预约与提交分离、回滚只减持有计数、在途预约数归零 |
| `S3` | 13 | 入队失败回滚 | Normal 环满后第 9 次 `RejectedFull`，排空一个槽位后同一时间戳重试被准入 |
| `S4` | 11 | 积压上限 | `max_backlog=2` 时第 3 次 `RejectedFull`（reason=5），critical 不豁免 |
| `S5` | 9 | 默认关闭不变性 | 策略关闭时 8 次快速提交全部入队，且 `NoAdmission` 占 1 字节 |
| `S6` | 15 | 跨目标隔离 | 目标 1 被区间与上限双重限制时，目标 2 连续三次入队不受影响 |
| `S7` | 15 | 快照环 | 8 槽环形缓冲淘汰 seq=1，`Expected` 错误码区分空、越界与已淘汰；写者最后发布 seq，读者前后校验，混合记录报 `kTorn` |
| `S8` | 22 | 真实中断抢占 | timer2（10 MHz，IRQ 28 → vector 44）在提交中途打断线程，准入总数等于 1 + 8 个 tick，出队顺序与入队顺序一致 |
| `S9` | 12 | 尺寸 | `ArrivalGate` 24 字节（无原子量）、策略对象 = 临界区钩子 + `(kMaxAo+1)` 个门、协调器增量不超过策略对象、快照环为显式配置的存储 |
| `S10` | 11 | 区间 + 上限同时生效 | 上限拒绝时区间预约被回滚，同一时间戳的重试被准入 |
| `S11` | 22 | 派发突发不误判 | 两个目标在同一时间戳各自准入；在 t=500000 一次性批量排空 4 个槽位后，窗口仍停在到达时间 1000，t=2000 的下一个到达照常准入 |
| `S12` | 19 | 过载保护升级与恢复 | 三次超预算的 direct 派发把 AO 推到 `BrokenL1`（direct 关闭、排队仍受理），冷却 + 低水位 + 三个健康探测窗口后回到 `Normal` 并恢复 direct |
| `S13` | 19 | 逐 AO 隔离 | 真实入队路径把 Normal 车道填满触发 overflow，目标 1 进入 `BrokenL2` 而目标 2 保持 `Normal` 并继续走 direct；critical 事件不被过载门丢弃，只失败在车道容量 |
| `S14` | 21 | 事件池引用记账 | `EventPool<64,8,RttSingleCoreProfile>` 在真实 PRIMASK 临界区下：排空后 `used()` 回 0，多引用事件在最后一次释放前不被回收，池满时拒绝分配，`shutdown()` 后不再分配 |

S8 使用该模型上唯一可用的异步中断源：`stm32f4.repl` 的 timer2 挂在
`nvic@28`，而该模型不实现 SysTick（写 `0xE000E010` 被记为 unhandled）。
排空必须每 `kBatchSizeMax` 调用一次 `begin_batch()`：`batch_used_` 只在
`begin_batch()` 中复位，扁平循环会在批次上限处停止，把完好的队列误报为
截断。

## 5. 判定规则

`run.sh` 依次检查：

1. 编译期探针通过（`StaticAdmission`/`AdmissionReason`/`ArrivalGate`/
   `Expected` 存在），否则退出码 2，不允许静默降级为“仅基线”；
2. Renode 进程返回 0 且 `uart.log` 非空；
3. 输出中不存在以 `FAIL ` 开头的行，且存在 `RESULT PASS n/m` 满足 `n == m`；
4. 断言总数必须等于固定值：完整夹具 231，仅基线 15。`n/m` 由目标端同一个计数器生成，删掉若干断言后仍是 `n == m`，所以总数单独钉死；
5. 每个期望组（`baseline`、`S1`..`S14`）的 `PASS <组>.` 条数必须等于 README 表里的固定值，删掉组内任意一条都会失败。

第 4、5 条同时防止“少跑用例仍然全绿”。新增或删除断言时必须同步更新
`run.sh` 里的 `EXPECTED_TOTAL`/`GROUP_COUNT` 和本文表格，否则夹具会拒绝通过。

`wdg.sh` 额外检查 `.noinit` 段非空、Renode 日志出现
`Watchdog reset triggered`、且只发生两次启动（第三次启动意味着健康对照组
也被复位）。`mpu.sh` 有两道门，见第 7 节。

## 6. 变异测试

`mutate.sh` 把 `include/coact` 复制到 `mktemp` 目录，用 Python 精确替换一处
源码（匹配数必须为 1，否则报“源码形状已变化”），再以 `COACT_INCLUDE_DIR`
指向副本调用 `run.sh`，要求它失败。真实头文件不被修改。

每个变异使用独立的 `COACT_RENODE_BUILD_DIR`，不会覆盖主夹具的 ELF 与
`uart.log`；判定条件除退出码非零外，还要求日志里出现目标端的 `FAIL ` 行。
编译失败或夹具自身报错不算“变异被捕获”，会以 `MUTATION ERROR` 报出。

| 变异 | 文件 | 破坏的不变量 | 首先捕获它的断言 |
|------|------|--------------|------------------|
| `gate_interval` | `admission.hpp` | 区间比较恒假 | `S1.t999_reject` |
| `reservation_rollback` | `admission.hpp` | 预约失败不回滚窗口 | `S3.retry_same_ts_admitted` |
| `commit_rolls_back` | `admission.hpp` | 成功提交也回滚 | `S1.t999_reject` |
| `direct_timeout_ignored` | `coordinator.hpp` | 超预算不再上报 | `S12.l1_after_three_overruns` |
| `overload_drops_critical` | `coordinator.hpp` | 过载门丢弃 critical | `S3.retry_same_ts_admitted` |
| `breaker_lookup_not_per_ao` | `monitor.hpp` | 熔断器查找恒指目标 1 | `S13.t2_direct_unaffected` |
| `shared_arrival_gate` | `admission.hpp` | 所有目标共享一个门 | `S11.t2_at_0` |
| `pool_used_not_released` | `pool.hpp` | 回收不减 `used` | `S14.used_after_drain` |
| `rollback_restores_timestamp` | `admission.hpp` | 回滚写回预约时间戳 | `S2.gate.last_after_rollback` |
| `hold_never_released` | `admission.hpp` | 失败的预约不释放持有 | `S2.gate.holds_while_reserved` |
| `backlog_cap_ignored` | `admission.hpp` | 积压上限恒为 65535 | `S4.third_rejected` |
| `snapshot_ring_ignores_config` | `monitor.hpp` | 快照环强制 8 槽，绕过配置探测 | `S9.snapshot_ring_costs_storage` |
| `snapshot_eviction_ignored` | `monitor.hpp` | 回绕序号不再报 `kEvicted` | `S7.evicted_fails` |

第 6 项说明一个容易踩的坑：协调器在提交路径上通过
`detail::target_breaker(bank, target)` 取到具体 `Breaker` 再调用
`on_overflow()`，并不经过 `BreakerBank::on_overflow()`。变异必须打在真实
路径上，否则会得到“变异未被捕获”的假阴性。

原子后端另有一条独立证据：对主夹具 ELF 反汇编后按调用点归因，`ArrivalGate`
的提交路径没有任何 `__atomic_*_8` 调用。框架代码里的 64 位原子调用点只出现在
`AdmissionSnapshotRing::record()`/`lookup()`（`fetch_add_8`、`load_8`、
`store_8`）：这是显式启用的诊断路径，由 `runtime.c` 的关中断 64 位实现承载。
其余 `__atomic_load_8` 调用在夹具自己的断言里读取 64 位计数器，不属于准入
判定。准入判定本身只使用普通字段，并在 PAL 临界区内完成读改写。

## 7. MPU 能力矩阵

`mpu_main.cpp` 对每项能力都配一个对照访问，使故障可以归因到被测配置，而不是
归因到“模型本来就不允许访问 RAM”：

| 能力 | 对照 | 实测 |
|------|------|------|
| 无访问区写/读 | MPU 关闭时同一地址可写 | MemManage，`CFSR=0x82`，`MMFAR` 命中 |
| 特权写只读区 | 同一区可读 | `ENFORCED` |
| RAM 执行 | 同字节在非 XN 区可执行 | `ENFORCED` |
| XN 区执行 | 同上 | `ENFORCED`，`CFSR=0x01` |
| 重叠区优先级 | 关闭高优先级区后同一访问放行 | `ENFORCED` |
| 栈守卫 | 独立处理栈（MSP 在 RAM 顶部） | `ENFORCED`，`MMFAR=0x200008f4` 落在 256 字节守卫内 |
| 非特权访问 | 同一访问在特权态放行 | `ENFORCED`（informational） |

两个实现细节决定了这个矩阵能否跑完：

- 真实的深递归溢出会让线程 SP 落进守卫区，异常入栈本身失败
  （`UsageFault` + `MSTKERR`，`CFSR=0x00010092`），没有可用的返回帧。夹具
  因此在处理程序里切换到干净的 MSP 并从 `guard_tail()` 收尾，这正是产品
  故障策略会做的事（复位/终止该上下文），而不是假装能返回。
- 递归函数必须禁止尾调用优化（`optimize("O0")` 且递归后仍使用局部数组），
  否则 `-Os` 把它变成常量栈循环，守卫永远不会被触及，矩阵会输出一个假的
  `NOT_ENFORCED`。
- 非特权探针之前必须给 flash 与 MSP 窗口各建一个允许非特权访问的区：
  `PRIVDEFENA` 的背景映射只覆盖特权访问，否则第一条取指就故障，数据访问
  探针根本跑不到。

`mpu.sh` 有两道门：第一道要求无 `FAIL` 行且存在 `RESULT MPU PASS n/n`；
第二道把 `CAP` 行与冻结的 `mpu_caps.expected` 做 diff。能力集变化不是
“通过/失败”问题，而是“这个模型还能被引用来说明什么”的问题，因此漂移必须
由人重新推导设计结论后再冻结。当前冻结值：11 项能力全部 `ENFORCED`，其中
8 项标记为 `required`（设计 5.1/8.1 依赖），3 项为 `informational`。

## 7.1 FreeRTOS PAL 夹具（真实内核）

`freertos.sh` 不用 stub：它把 FreeRTOS Kernel V11.1.0 的 `tasks.c` / `queue.c` /
`list.c` 与 `portable/GCC/ARM_CM3` 端口一起编进 Cortex-M4 镜像，在 Renode 里
跑真实调度器。选 CM3 端口而不是 CM4F，是为了避开 FPU 上下文汇编，同时保留
同一套 ARMv7-M 的 SVC/PendSV/SysTick 与 BASEPRI 临界区路径。

启动顺序按设计 D9：`main()` 只创建 bootstrap 任务并调用
`vTaskStartScheduler()`（不返回）；bootstrap 在调度器运行后执行
`pal.initialize()`、`register_current_task()` 和 `Runtime::start()`。
静态构造由 `start_freertos.c` 遍历 `.init_array` 完成。

32 条断言覆盖：

| 组 | 内容 |
|---|---|
| `FR.pal_*` / `FR.context_*` | 显式初始化、栈尺寸、逻辑优先级映射、ContextSlot 去重 |
| `FR.clock_*` | SysTick 真实推进（`vTaskDelay(5)` 后纳秒数前进 >= 5 ms） |
| `FR.sem_*` / `FR.mutex_*` | 计数信号量、非阻塞/有限超时、mutex 往返 |
| `FR.isr_*` | TIM2（IRQ 28，优先级 6）真实中断里调用 `softirq_raise()` 与 `sem_release_from_isr()`，任务侧收到 payload=7 和信号量 |
| `FR.thread_*` | 常驻 worker 创建、执行、join、复槽 |
| `FR.runtime_*` | Runtime + Staging + EventPool 端到端：8 个事件全部派发，停止后池占用归零 |
| `FR.race_*` | 两个 FreeRTOS 任务在真实抢占下各做 100 次 `create → join`，执行次数、成功次数、两个任务各自完成数一致 |
| `FR.dispatcher_stack_watermark` | `configCHECK_FOR_STACK_OVERFLOW=2`；dispatcher 剩余栈 976/1024 字（要求 >= 25%），溢出钩子会打印 FAIL 并停住 |

实测：`RESULT PASS 32/32`，`text=23100 bss=64076`，反汇编中
`__atomic_compare_exchange_8` 调用点为 0。`bss` 偏大来自 4 个 1 KiB worker 栈、
EventPool 与 Runtime 的固定数组，不是堆。

`freertos_wdg.sh` 是独立的四阶段故障夹具，`.noinit` 跨复位记账：

1. 任务通过 `watchdog_progress()` 连续踢 IWDG 40 ms（窗口 32 ms），随后
   `cpsid i` 关中断并死循环；IWDG 复位，实测 29.862 ms。
2. 复位后报告延迟，再用 MPU 无访问区触发 MemManage（`CFSR=0x82`，
   `MMFAR=0x20020000`），处理程序打印现场并复位。
3. 复位后报告故障恢复，再触发 `configASSERT`，断言钩子走同一复位路径。
4. 复位后报告断言恢复并输出 `RESULT PASS 8/8`。

## 8. 本夹具不证明什么

- 不证明真实 STM32F407 或产品板上的行为。Renode 是功能模型：MPU 矩阵说明
  该模型强制执行了哪些访问属性，不构成硅片证据。
- 不证明 SMP 并发正确性。目标程序单核；`runtime.c` 的 64 位原子是关中断的
  读改写，不是 `ldrexd/strexd`。S8 覆盖了单核真实中断抢占，不覆盖多核。
- 不证明快照环在真实并发下的撕裂行为。Renode 夹具是确定性单核，读者不会
  与写者交错；`kTorn` 分支由 host 测试的并发读写用例覆盖（4 万次记录、
  单读者循环，实测 `mixed=0`）。
- 不证明真实时序。虚拟时钟由测试推进；S12 的“超预算”靠在动作里推进虚拟
  时钟实现，不涉及指令级时间。tick、抢占延迟、低优先级老化不在覆盖范围。
- 不证明调度器与 Dispatcher 线程。夹具按 Dispatcher 的方式调用
  `begin_batch()/dequeue_one()`，并直接给熔断器喂 Dispatcher 会喂的输入；
  没有真正的 Dispatcher 线程在跑。
- 不证明其他 `Config`。容量、批次上限、冷却周期固定为本夹具的 `Cfg` /
  `IsrCfg`，改参数需重跑。
- 不证明链接脚本之外的内存约束，例如产品板的 MPU 条目预算、RTOS 已占用的
  区域、真实栈深度。
- 看门狗夹具证明的是“IWDG 在该模型上于约 32 ms 终止一个永不返回的动作，
  且每秒 kick 的对照组在 100 ms 内不被复位”，不证明产品板的看门狗时钟源
  与复位行为。
- FreeRTOS 夹具证明的是 V11.1.0 CM3 端口在该模型上的调度、SysTick、真实
  TIM2 中断进入 PAL 的 ISR 分支，以及固定静态资源下的端到端管线。它不证明
  CM4F/FPU 端口、厂商 BSP、tickless/低功耗配置、栈水位、长时间稳定性，也不
  证明目标板的 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 与中断优先级规划。
- 故障夹具证明的是该模型上的 IWDG 复位、MemManage 现场与复位、断言复位路径；
  不证明产品板的看门狗时钟、复位后输出安全、故障注入覆盖率或认证结论。

## 9. 文件

| 文件 | 作用 |
|------|------|
| `start.c` | 向量表、`.data` 复制、`.bss` 清零、跳转 `main`；未用槽位指向 `Default_Handler`，异常中断表现为可见挂死 |
| `link.ld` | FLASH 128 KB / RAM 192 KB；`.noinit (NOLOAD)` 供看门狗夹具跨复位保存计数与时间戳 |
| `runtime.c` | `memset/memcpy` 与关中断实现的 64 位原子 |
| `pal_baremetal.hpp` | 可推进的单调时钟；信号与 direct 钩子为空操作 |
| `output.hpp` | USART2 轮询输出与最小十进制/十六进制格式化 |
| `main.cpp` | 基线与 S1..S14 场景 |
| `wdg_main.cpp` | 看门狗夹具：跑飞 → IWDG 复位 → 量测延迟 → 健康对照组 |
| `mpu_main.cpp` | MPU 能力矩阵（10 个探针，每个带对照） |
| `freertos_main.cpp` | FreeRTOS PAL 夹具：真实内核、SysTick、TIM2 ISR、Sem/Mutex/Worker/Runtime |
| `freertos_wdg_main.cpp` | FreeRTOS 故障夹具：IWDG、MemManage、configASSERT、跨复位记账 |
| `start_freertos.c` | FreeRTOS 向量表（SVC/PendSV/SysTick/TIM2）与 `.init_array` 初始化 |
| `freertos/` | FreeRTOS Kernel V11.1.0 裁剪副本（MIT，见其 README） |
| `coact_admission.resc` / `coact_wdg.resc` / `coact_mpu.resc` | 三个夹具的 Renode 脚本 |
| `coact_freertos.resc` | FreeRTOS 夹具模板，`freertos.sh` 替换路径占位符 |
| `coact_freertos_wdg.resc` | 故障夹具模板，带重启宏，`freertos_wdg.sh` 替换路径 |
| `mpu_caps.expected` | 冻结的能力集，`mpu.sh` 第二道门的比对基线 |
| `run.sh` / `wdg.sh` / `mpu.sh` / `freertos.sh` / `freertos_wdg.sh` / `mutate.sh` | 构建、运行、判定 |
