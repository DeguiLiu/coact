# isp_pipeline_demo 功能测试报告

**结论**：68 项自检断言全部通过（exit 0）。测试方法为程序自检：运行结束时逐项核对结果与不变量，覆盖 RS500 业务场景的架构级复现与故障修复。

**关联文档**：架构说明见 `example_ISP_pipeline_design_architecture_zh.md`；故障处理见 `example_ISP_pipeline_design_consistency_avoidance_zh.md`；逐阶段运行输出见 `isp_pipeline_demo_run_log_fresh.txt`。

## 1. 测试范围

```mermaid
flowchart LR
    subgraph BOOT["上电出图功能链（Preview Start）"]
        direction LR
        A1["IRSC 4 步命令<br/>init/start/ctrl/output_enable"]:::f1 --> A2["ISP 双增益并行<br/>low/high + HL 融合"]:::f1
        A2 --> A3["PIC/TEMP 双流打包<br/>节点序列→SOUT→OUT"]:::f1
        A3 --> A4["输出接口<br/>WRAPE→USB Bulk / MIPI TX"]:::f1
    end
    subgraph ASSERT["自检断言五组（68 项）"]
        direction TB
        B1["链路不变量<br/>帧计数/帧序连续"]:::f2
        B2["数据面字节级校验"]:::f2
        B3["一致性故障复现+修复<br/>U1-U9 + 花屏"]:::f3
        B4["worker 交互协议<br/>命令-中断确认闭环"]:::f2
        B5["资源回收<br/>事件池归零"]:::f2
    end
    BOOT -->|"逐环节核对"| ASSERT
    A3 -.->|"复现故障"| B3
    B3 -.->|"验证修复"| A3
    classDef f1 fill:#dbeafe,stroke:#2563eb,color:#1e3a8a
    classDef f2 fill:#dcfce7,stroke:#16a34a,color:#14532d
    classDef f3 fill:#fee2e2,stroke:#dc2626,color:#7f1d1d
    style BOOT color:#1f2937
    style ASSERT color:#1f2937
```

功能链（蓝）逐环节对应断言组（绿 = 不变量，红 = 故障复现与修复）。

## 2. 测试方法

### 2.1 程序自检

main() 末尾逐项核对运行终态与不变量，任一失败打印 `[FAIL]` 并以非零退出码结束。断言分五组：

1. **链路不变量**：各环节帧计数一致（IRSC 产帧 30 → 增益/融合/增强/打包 → WRAPE 封帧 45 → 主机观察者收帧 45），帧序无缺失。
2. **数据面字节级校验**：像素按确定性公式逐节点变换，WRAPE 入口重新读取 DDR 复算比对（byte_mismatch==0）；读侧校验槽位帧戳防止读到被覆盖的旧槽位。
3. **一致性故障复现与规避**：U1-U9 与花屏逐类"先复现、再验证修复"。如参数回灌断言产测值 0xBEEF 被旧影子值 0x1002 覆盖（故障侧）、修复后零漂移；T37 提前封帧断言截断字节数恰为 655360（与真实抓包一致）。
4. **worker 交互协议**：7 实例 executed/rejected 计数核对；命令通道 executed==4（命令与中断回执一一对应）；USB 完成路径经软中断转发零丢失。
5. **资源回收**：事件池终态 used==0；AO 在途等待计数归零（停机排空不丢完成事件）。

### 2.2 日志交叉验证

结构化日志（事件码+参数）与 printf 输出数值一致（如事务计数 6840 对应日志 a0=0x1ab8），断言失败可从结构化轨迹回溯。

### 2.3 日志范围

最新运行输出 727 行，分四层：

| 层级 | 内容 | 频次 |
|---|---|---|
| 结构化日志 | HSM 状态迁移轨迹 | 233 条，全量 |
| 关键事件 | 会话/重配/worker 统计/帧完成 | 约 30 条 |
| 生命周期与里程碑 | worker 启动/排空/退出 + 每 10 帧进度 | 27 条 |
| 拒绝告警 | 提交被拒逐次输出 | 正常为 0；出现即预示丢帧 |

全量记录只放在 HSM 轨迹（信息最紧凑）；帧进度每 10 帧一条；单次运行总量约 300 条，可完整审计。

## 3. 测试边界

host 模拟不等价板级行为（时序为建模值）；flash_proxy_demo 已单独注册 ctest。

## 4. 复现命令

```bash
cd build && make -j12
./examples/isp_pipeline_demo            # 期待 RESULT: ALL PASS, exit 0
```

## 5. 与 RS500 功能对应关系

映射范围限核心模块与 vdcmd 命令通道，逐项基于源码与运行日志核实。对齐分级：完整（含故障边界复现）/ 语义对齐（交互模式等价）/ 结构对齐（拓扑一致）。

| RS500 功能 | 示例落点 | 对齐度 | 已知简化 |
|---|---|---|---|
| Preview Start 四阶段 | 编排器 + 会话状态机 | 完整 | Phase1/3 为占位 |
| IRSC 4 步命令序列 | 命令表 + IrscDriver AO | 完整 | 无寄存器位定义 |
| ISP 高/低增益并行 + HL 融合 | 双增益 AO + 融合 AO | 语义对齐 | 增益为数学变换；节点初始化简化 |
| PIC/TEMP 双流打包 | VideoFsm 乘积状态表 + VideoPack AO | 完整 | 节点算法为字节公式 |
| 一级 DMA 三帧循环 | SoutDma worker（环深 3） | 结构对齐 | 槽位+帧戳模型 |
| 中断事件向上 | worker 完成事件回 AO | 语义对齐 | 无真实 ISR 上下文 |
| vdcmd 命令通道 | CmdDma worker | 完整 | 无物理传输 |
| MIPI CSI TX 中断 | MipiIrq worker + MipiSink AO | 语义对齐 | 无 CSI-2/D-PHY |
| T37 提前封帧与 Identity Zoom 规避 | WRAPE 字节级复现 + 10 轮稳定 | 完整 | 最接近真实故障 |
| 超分 X1→X2 原子重配 | RecfgOrch 8 态 + 冻结窗口 | 完整 | 重配内容为几何参数 |
| deinit 停稳机制差异 | QuiescePolicy 编译期选择 | 完整 | ioctl 计数为建模值 |
| regmap 三标志缓存协议 | PeriphRegCache + RAII 旁路 | 完整 | 地址空间为最小示例集 |
| 三类画面异常 | 异常场景组断言 | 完整 | 像素为 8x8 小尺寸帧 |

### 5.2 同步与一致性问题对照

| RS500 记录的问题 | 处理机制 | 验证 |
|---|---|---|
| 计算口径不一致（X2 出图剩 1/4） | 单一 FrameGeometry 权威 + 冻结窗口 | 复现 + 回滚断言 |
| 新旧帧交替 / 提前封帧（T37） | 8 态冻结 + Identity Zoom | 字节级复现 + 10 轮稳定断言 |
| 参数回灌 | 三标志协议 + RAII 配对旁路 | 覆盖复现 + 零漂移断言 |
| 废弃地址 | layout_version 查询时作废 | 过期查询不命中断言 |
| 坐标失同步 | 单一坐标变换入口 | [100,200]→[439,539] 断言 |
| deinit 停稳分叉 | QuiescePolicy 统一路径 | ioctl 计数对比断言 |
| 峰值时延突破缓冲窗口 | 显式窗口模型 + 溢出守卫 | 尖峰丢帧复现 + 深缓冲零丢帧 |
| 未停稳即重启闪屏 | HSM 停稳弧 | 旧几何复现 + 对齐断言 |
| 花屏（位宽错配） | 位宽假设单点验证 | 损坏复现 + 零损坏断言 |
| 并发同步（worker 交互） | 单写路径 + 忙则拒绝 + 命令-中断闭环 | executed==4、pool.used==0 |

机制归结为三项约束：**单一写入者**、**事件事务边界**、**明确的完成确认**。结果只说明 host 模拟通过，不代表板级行为。

### 5.3 已知偏差

- 事务窗口关闭已由终局动作自动驱动（此前由主线程轮询驱动，现已修复）
- 硬件为行为模拟，不含寄存器位定义与物理传输；时序为建模值
