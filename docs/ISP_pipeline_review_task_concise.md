# coact 评审任务（精简版）

**对象**：提交 f8bd107 及后续未提交改动（include/coact/ + examples/isp_pipeline/ + src/core/test_*）
**模式**：只读评审，不修改任何文件，产出中文评审报告
**边界**：demo 定位是架构模型与故障注入演示（消息发生级）；coact 定位单一（AO 事件驱动框架）——不要把无真实寄存器位定义/SPI 传输/线程全覆盖当缺陷，不要建议扩平台能力面。

## 评审内容

1. **架构**：AO/worker 边界纯度（业务逻辑不得泄漏进 worker）；三块黑板五要素契约（写者/读者/同步/失效/所有权）闭环；HSM 层次与会话门控、事务窗口终局自驱覆盖；PAL SemOps/CondOps/ThreadOps 契约对称性与 RT-Thread 复刻语义正确性；coro 栈池边界与 RT-Thread 编译期隔离。
2. **并发**：EventPool tagged-CAS ABA 防护；submit_from_task/isr 使用；DdrCtx 槽位 claim/release 窗口；coro 切点对象生命周期。
3. **代码质量**：对照 docs/cpp_coding_conventions_zh.md 47 项（固定宽度整型/常量左侧/Allman/零堆/无 goto/无 RT_ASSERT）；找过度设计；hpp 非 inline 非模板定义的 ODR 风险。
4. **测试与文档**：main.cpp 62 断言是否真断言（找恒真/弱断言）；ctest 覆盖缺口；docs/design_architecture_zh.md 与代码漂移点（抽查 3-5 处核实）。

## 已知线索

评审时 coro/SoftIrq/BitFieldView/vocabulary 四个实施 agent 在跑，报告对照工作树实际状态，不假设已合并。已知框架缺口：Monitor pending 只记提交侧不递减；cond_broadcast 快照实现有唤醒丢失窗口（潜在）；RtThread() 默认 ctor 引入 16KiB 静态资源。

## 输出格式

1. 总评（3 行内：健康度一句话 + 最严重 1 个问题）
2. 发现清单：Critical/Major/Minor 排序，每条含 文件:行号、问题、失败场景、修复建议
3. 架构建议 ≤5 条
4. 规模 10-20 条，宁缺毋滥，每条必须核实，不确定标 [待核实]
