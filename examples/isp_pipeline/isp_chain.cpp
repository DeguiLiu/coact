/*
 * ===========================================================================
 * isp_chain.cpp —— isp_chain.hpp 的实现 TU（增益链/融合/Enhance/TPD）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 isp_chain.hpp 的实现 TU。GainNodeBase<Policy>、HlFuseAO、
 *     EnhanceAO / TempChainAO（FusedNode）以及 IspIrqWorker / SoutDmaWorker
 *     / MipiIrqWorker 三个非 AO worker 的 action 与骨架全部为模板可见 /
 *     hpp 内联实现，本 TU 承载将来非模板实现移入位（节点延迟表外置、
 *     IRQ 仿真侧外置、模拟时序去宏化等）。
 *   - 保留本文件以固定"每模块一对 .hpp/.cpp"结构，并给链接器一个稳定的
 *     符号实例锚点。
 *
 * 与其他文件的关系
 *   - 仅 include isp_chain.hpp（从而带入 common.hpp 与 sensor_irsc.hpp 的
 *     Sig / DdrCtx / DemoWorkerBase / g_pal / PoolT / Rt 等词汇）；不直接
 *     include 其他业务文件。
 *   - 链接时被 isp_pipeline_demo 显式列出；本 TU 不提供任何外部可见符号，
 *     但保证 worker 类与 HSM 表的可实例化编译通路在最终镜像中闭合。
 *   - 依赖：isp_chain.hpp 暴露的全部 ISP 节点 / 中断 worker / DMA worker
 *     API；语义不增不减。
 *
 * 文字图（与 hpp 视角一致，标注 TU 角色）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                              (hpp：本TU是                        │
 * │                               它的实现TU) [video_stream]──kPicPacked──▶ │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */

#include "isp_chain.hpp"

