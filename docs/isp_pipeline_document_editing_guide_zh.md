# ISP Pipeline 文档编写与整理指导

## 1. 目的

本文总结 `isp_pipeline` 文档最近一轮整理采用的方法，适用于架构说明、故障分析、功能测试报告和运行日志。目标不是增加术语和图表数量，而是让读者能够按固定顺序回答三个问题：

1. 系统处理什么业务？
2. 各模块如何协作？
3. 日志和断言如何证明结果？

## 2. 文档分工

| 文档 | 首要回答的问题 | 不应承担的内容 |
|---|---|---|
| `example_ISP_pipeline_design_architecture_zh.md` | 业务流程、模块职责、状态变化和协作关系 | 详细测试结果、完整代码实现清单 |
| `example_ISP_pipeline_design_consistency_avoidance_zh.md` | 故障如何产生、如何处理、如何避免复发 | 全量模块架构介绍 |
| `example_ISP_pipeline_functional_test_report_zh.md` | 测试范围、断言分组、复现命令和边界 | 逐行解释业务源码 |
| `example_ISP_pipeline_review_record_code_architecture.md` | 评审检查什么、使用哪些标准 | 代替评审结论 |
| `example_ISP_pipeline_review_record_code_architecture.md` | 当前代码问题、风险和处理建议 | 代替功能测试报告 |
| `isp_pipeline_demo_run_log_fresh.txt` | 某次最新程序运行的实际过程和结果 | 代替设计文档解释系统原理 |

每份文档开头应列出关联文档，并说明本文件的阅读范围。读者无需在多个文件中猜测信息归属。

## 3. 推荐章节顺序

架构文档应按以下顺序组织：

1. **业务流程**：一帧数据如何产生、处理、封装和输出。
2. **配置切换与故障处理**：X1→X2 如何检查、停稳、应用、确认和回滚。
3. **运行角色与状态**：AO、worker、消息中心和共享数据的职责。
4. **验证入口**：只保留结果索引，详细证据放测试报告和运行日志。
5. **实现说明**：仅在读者确实需要时补充代码结构。

业务内容必须先于代码内容。不要在读者尚未理解帧流和重配目的前，直接介绍 CRTP、HSM 表、模板参数或 `ucontext`。

## 4. 术语表达

### 4.1 首次出现时给出中英文

AO、worker、Dispatcher 等术语首次出现时同时给出中文含义：

| 写法 | 推荐说明 |
|---|---|
| `Orchestrator / 启动编排器` | 收集启动回执并推进流程 |
| `RecfgOrch / 重配编排器` | 管理 X1→X2 配置切换 |
| `LowGain / 低增益` | 低增益图像处理 |
| `VideoFsm / 视频状态机` | 管理 PIC/TEMP 启停 |
| `Wrape / USB 封帧` | 判断帧长并生成 EOF |
| `WinHost / 主机观察` | 记录主机收到的帧 |

后续可以只使用英文标识，但图表和表格应保留中文用途说明。

### 4.2 避免只写抽象分类

不要只写“事件传播”“提交边界”“结构性收束”等概念。应补充动作和结果，例如：

- “收到 `kSoutIdle` 后才能写入新参数”；
- “首帧长度不匹配时恢复旧快照”；
- “worker 完成后通过 `submit_from_task()` 通知目标 AO”。

## 5. 队列和拓扑表达

必须区分两类队列：

| 队列 | 数量和拥有者 | 内容 | 处理者 |
|---|---|---|---|
| AO 事件队列 | 14 条，每个 AO 一条 | `kFrameIrscOut`、`kSoutDone`、`kFrameEof` 等业务事件 | Dispatcher 派发给目标 AO |
| worker 输入队列 | 6 条，分别属于 CmdDma、IspIrq×2、Sout、Mipi、Usb | job、命令或单槽任务 | 对应 worker 执行 |

`IrscWorker` 是第 7 个 worker，按帧节拍自主产帧，没有输入 job 队列。

图中必须画出完整方向：

```text
AO action --submit(job)--> 对应 worker 输入队列 --> worker 执行
worker --submit_from_task(event)--> Dispatcher --> 目标 AO 事件队列
```

“worker 输入队列”不能画成脱离 worker 的独立节点，也不能称为 AO 队列。图注应明确说明：7 个 worker 中 6 个拥有独立输入队列，队列彼此不共用；14 个 AO 的事件队列由 Dispatcher 管理。

## 6. 黑板关系

三块黑板应同时说明业务用途、写入方和读取方：

| 黑板 | 业务内容 | 写入方 | 读取方 |
|---|---|---|---|
| 硬件状态 | zoom、SEL、WRAPE 帧长、会话状态 | 编排器/重配流程 | 封帧逻辑、状态 guard |
| DDR 帧数据 | 像素、帧号、槽位状态 | 对应处理阶段 AO | 下游 AO、WRAPE、MIPI |
| 寄存器镜像 | shadow、hardware、地址版本 | 唯一 `write()` 入口 | `audit()`、地址查询 |

图中应明确区分 AO 和普通 worker：AO 可以写业务黑板；worker 只产生完成事件，不直接写黑板。若 worker 使用 mutex/cond，只表示它在等待自己的输入任务，不表示它进入 AO 事件队列。

## 7. Mermaid 图规范

### 7.1 每张图只表达一个主要关系

- 数据流图：只表达帧处理顺序。
- 重配图：只表达请求、检查、停稳、应用、首帧确认、提交/回滚。
- 拓扑图：表达 worker、队列、消息中心、AO 的连接。
- 黑板图：表达 AO/worker 与三块黑板的读写关系。

不要把所有状态、事件码、代码函数和颜色含义堆进同一张图。

### 7.2 颜色保持一致

建议统一颜色语义：

| 颜色 | 含义 |
|---|---|
| 黄色 | 输入、等待或停稳条件 |
| 蓝色 | 正常处理、编排或 AO |
| 青色 | 数据应用或 DDR |
| 紫色 | Dispatcher、检查或寄存器镜像 |
| 绿色 | 输出、成功或提交 |
| 红色 | worker、错误、拒绝或回滚 |

每份文档首次出现颜色图时，应附一行图例。颜色表达角色或结果，不应同一文档内反复改变含义。

### 7.3 图注写法

图注只回答“这张图说明什么”，不重复整段正文。例如：

> 图：AO action 将 job 放入对应 worker 的输入队列；worker 完成后将事件送入 Dispatcher，再派发给目标 AO。

## 8. 运行日志整理

### 8.1 fresh 日志必须来自最新程序

fresh 日志不能从旧日志复制。每次代码或构建配置变化后，应重新编译并执行：

```bash
g++ -std=c++17 -O0 -DISP_DEMO_CORO -DCOACT_RTT_STUB \
  -Iinclude -I. -Iexamples \
  examples/isp_pipeline/main.cpp \
  examples/isp_pipeline/{sensor_irsc,isp_chain,video_stream,output_itf,recfg_session}.cpp \
  src/core/pal_posix.cpp src/diag/log_rtthread.cpp \
  -o /tmp/demo_coro -lpthread
timeout 60s /tmp/demo_coro > /tmp/isp_coro_raw.log 2>&1
```

执行结果应记录：退出码、总行数、`[PASS]` 数量和最后一行 `RESULT`。日志头部必须注明是否为 `ISP_DEMO_CORO`，避免把 pthread 输出误当作 coro 证据。

### 8.2 中文说明的插入位置

中文说明应穿插在原始输出对应阶段之前，至少覆盖：

- 协程模式和 worker 数量；
- Phase1/3/4 启动阶段；
- IRSC 四步命令和 ISP 八节点；
- 产帧阶段的协程切换验证；
- X4/DMO 拒绝、故障回滚和 X2 提交；
- T37 提前 EOF 复现及修复；
- 寄存器缓存、错误注入和三类画面异常；
- 逆序停机、队列排空和最终断言。

说明应写成“处理阶段 + 关键条件 + 可核对结果”，避免只写口号。

### 8.3 原始输出完整性

中文说明之外的原始输出必须保持不变。可用以下方式核对：

```bash
awk 'BEGIN{raw=0} /^\[worker\] cmd_dma started/{raw=1} raw && $0 !~ /^【/{print}' \
  docs/isp_pipeline_demo_run_log_fresh.txt >/tmp/fresh_without_comments.log
diff -u /tmp/isp_coro_raw.log /tmp/fresh_without_comments.log
```

无差异才说明日志注释没有篡改程序证据。

## 9. 章节删减规则

删减时优先删除以下内容：

1. 同一结论的重复图注和重复段落；
2. 读者暂时不需要的源码函数清单；
3. 已在其他关联文档完整说明的测试细节；
4. 历史评审过程和已废弃命名；
5. 没有输入、输出和结论的抽象术语。

不得删除以下核心内容：

- 主链路和停机顺序；
- 14 个 AO、7 个 worker 和消息中心拓扑；
- AO 事件队列与 worker 输入队列的区别；
- 三块黑板及其读写关系；
- X1→X2 提交/回滚条件；
- #41 协程修复的运行证据入口。

## 10. 完成检查表

提交文档前逐项检查：

- [ ] 业务流程位于代码实现说明之前。
- [ ] AO 名称首次出现时有中英文说明。
- [ ] 图中保留 14 个 AO、7 个 worker 和消息中心。
- [ ] 明确区分 14 条 AO 事件队列与 6 条 worker 输入队列。
- [ ] `IrscWorker` 明确标注为无输入 job 队列。
- [ ] 三块黑板分别标出写入方和读取方。
- [ ] 彩色图有统一图例，图注说明主关系。
- [ ] fresh 日志来自最新程序，而不是历史文件复制。
- [ ] fresh 日志包含中文阶段说明，且原始输出可逐字还原。
- [ ] 运行结果包含退出码、PASS 数量和最终 RESULT。
- [ ] 关联文档链接有效，未残留不应公开的本地路径。
- [ ] 行数、标题和 Mermaid 代码块通过格式检查。

## 11. 当前项目对应文件

本指导对应当前文档体系：

- 架构：[example_ISP_pipeline_design_architecture_zh.md](example_ISP_pipeline_design_architecture_zh.md)
- 一致性：[example_ISP_pipeline_design_consistency_avoidance_zh.md](example_ISP_pipeline_design_consistency_avoidance_zh.md)
- 功能测试：[example_ISP_pipeline_functional_test_report_zh.md](example_ISP_pipeline_functional_test_report_zh.md)
- 最新日志：[isp_pipeline_demo_run_log_fresh.txt](isp_pipeline_demo_run_log_fresh.txt)

