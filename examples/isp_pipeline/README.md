# isp_pipeline 代码层次与拓扑

RS500 红外视频链路架构演示（Linux host / RT-Thread 双平台）。定位：架构模型与故障注入演示，模拟消息发生级，不追求硬件行为复刻。

## 一、文件分层

```
┌─────────────────────────────────────────────────────────┐
│ main.cpp        场景编排 + 66 项自检断言（上层入口）        │
├─────────────────────────────────────────────────────────┤
│ 业务模块层（每模块 = AO 状态机 + worker 硬件代理）           │
│   sensor_irsc    产帧 + vdcmd 命令通道（RS500 sensor_input）│
│   isp_chain      增益双链/融合/打包（RS500 isp + video）    │
│   video_stream   视频流 FSM + 停稳策略（RS500 video）       │
│   output_itf     输出接口 + T37 裁决（RS500 outITF）        │
│   recfg_session  原子重配事务 + 会话门控（RS500 rte）        │
├─────────────────────────────────────────────────────────┤
│ common.hpp      共享词汇：Sig/IoMeta/DdrCtx/黑板/日志       │
│ coro_mode.hpp   coro 执行拓扑（Linux 可选，默认 pthread）   │
│ coro_pal.hpp    coro PAL shim（满足 DemoPal 接口）          │
└─────────────────────────────────────────────────────────┘
        │ 依赖 include/coact/（AO 框架 + PAL + coro + timer）
        ▼
```

依赖方向自上而下：main → 业务模块 → common → coact 框架。模块之间互不 include（跨模块词汇下沉 common.hpp）。

## 二、运行拓扑（14 AO + 7 worker 实例）

```mermaid
flowchart LR
    subgraph AOL["14 AO（Dispatcher 单线程串行）"]
        ORCH["OrchestratorAo"]:::high --> IRSC["IrscDriverAo"]:::n
        IRSC --> ENH["EnhanceAo"]:::n --> PACK["VideoPackAo"]:::n
        IRSC --> TPD["TempChainAo"]:::n --> PACK
        FSM["VideoFsmAo 9态"]:::n --> PACK
        WRAP["WrapeAo"]:::n --> HOST["WinHostAo"]:::n
        MIPI["MipiSinkAo"]:::n
        RECFG["RecfgOrchAo 8态"]:::n
        GAIN["LowGain/HighGain/HlFuse 3AO"]:::n
        IRSC --> GAIN --> PACK
    end
    subgraph WL["7 worker 实例（PAL 线程，硬件行为模拟）"]
        W1["IrscWorker"]:::w -->|"kFrameIrscOut"| IRSC
        W2["UsbDmaWorker"]:::w -->|"kFrameEof"| WRAP
        W3["CmdDmaWorker"]:::w -->|"kIrscDmaDone"| IRSC
        W4["IspIrqWorker x2"]:::w -->|"节点完成"| ENH
        W5["SoutDmaWorker"]:::w -->|"kSoutDone"| FSM
        W6["MipiIrqWorker"]:::w -->|"TX 中断"| MIPI
    end
    classDef high fill:#fecaca,stroke:#dc2626,color:#000000
    classDef n fill:#dbeafe,stroke:#2563eb,color:#000000
    classDef w fill:#dcfce7,stroke:#16a34a,color:#000000
```

- **AO 侧**：事件驱动状态推进（HSM），只做决策不做阻塞；读写寄存器/DDR 走等待态（提交→中断确认）
- **worker 侧**：`WorkerBase<Derived,Job,Depth,PalT>` CRTP 骨架，无锁 `SpscRing` 交接（Dispatcher 单生产者 + worker 单消费者，深度取 2 的幂）+ 单 PAL 信号量唤醒，满环忙则拒绝 + parking ring 按 id 匹配，完成事件回 AO（中断路径模拟）
- **coro 模式**（ISP_DEMO_CORO 宏开启）：worker 跑在单 pthread 绑核的 ucontext 有栈协程上，与 RT-Thread 单核公平对比

## 三、三块黑板（AO 与 worker 的数据交汇）

| 黑板 | 内容 | 同步机制 |
|---|---|---|
| g_hw_bb 硬件状态域 | zoom/SEL/WRAPE，SEL 帧长权威 | 单写者 + 原子标量 |
| DdrCtx 帧数据域 | 7 区×8 槽，FrameStamp placement new | SlotOwner 三态所有权（kFree/kWriterOwned/kReaderClaimed） |
| PeriphRegCache 寄存器镜像 | shadow/hardware 双份 + BitFieldView 位域 | regmap 三标志协议（cache_only/bypass/dirty） |

## 四、文件清单

| 文件 | 行数 | 角色 |
|---|---|---|
| common.hpp | ~950 | 共享词汇层（事件/黑板/PAL 选型/HSM 表） |
| sensor_irsc.hpp/.cpp | 699/37 | IrscWorker + WorkerBase 骨架 + CmdDmaWorker + IrscDriverAo |
| isp_chain.hpp/.cpp | 745/40 | 帧侧 worker + 增益双链 + HlFuseAo + VideoPackAo |
| video_stream.hpp/.cpp | 934/40 | VideoFsmAo 乘积状态表 + QuiescePolicy |
| output_itf.hpp/.cpp | 612/42 | 输出 sink + UsbDmaWorker + WrapeAo/WinHostAo |
| recfg_session.hpp/.cpp | 747/43 | RecfgOrchAo 8 态 + PeriphRegCache + BitFieldView 字段 |
| main.cpp | ~1300 | 场景编排 + 66 项断言 |
| coro_mode.hpp / coro_pal.hpp | 168/182 | coro 执行拓扑（可选编译） |

构建：`cd build && make -j12 && ./examples/isp_pipeline_demo`，期待 `RESULT: ALL PASS`。

## 五、双后端验证矩阵（design_isp_pipeline_optimization P5）

| 验证层 | host/POSIX（默认 + pthread 手工构建） | RT-Thread（ISP_DEMO_USE_RTT） |
|---|---|---|
| 构建 | pthread/coro；`-fno-exceptions -fno-rtti` 全程 | RTT stub 编译门（ctest isp_pipeline_demo_rtt） |
| Profile | HostSmpProfile（tagged-CAS 池 + 批量回收 + spin CS） | RttSingleCoreProfile（irq-mask 池 + 即时回收 + IRQ CS） |
| 队列 | SpscRing 顺序/压测（test_spsc_ring 24 用例含线程压测） | 同一 SpscRing（无平台分支） |
| worker 交接 | 真阻塞 sem（pthread）/ 协作 yield（coro） | rt_sem（静态） |
| 日志 | raw-hex sink（stdout） | 静态 writer 线程 + record_from_isr |
| 生命周期 | pool.used()==0 + diag 守恒断言 | 同左（编译级；真板待板级验证） |
| 覆盖 | ctest 53/53（含本 demo 68+ 断言） | 编译门 + 板级待办清单见设计文档 §5 |

验收记录（2026-09 系列，分支 feature/isp-pipeline-demo）：
- P0 Trace 修复（2baa919）、P1 Profile 统一（4376ef4）、P2 WorkerBase SPSC（dad821a）、P3 diag 启动顺序与守恒断言（db25075）、P4 静态资源预算表（cad9d8a）
- 板级待办：真 RT_CPUS_NR=1 配置、中断嵌套、counter 回绕、IWDG 联动、栈水位实测（P1 注释中 16K/4K 的降额依据）
