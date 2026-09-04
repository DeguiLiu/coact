/*
 * ===========================================================================
 * sensor_irsc.cpp —— sensor_irsc.hpp 的实现 TU（worker 骨架与 driver AO）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 sensor_irsc.hpp 的实现 TU。当前 WorkerBase、CompletionWorkerBase、
 *     PeriodicProducerBase、SoftIrqCompletionWorker 与
 *     IrscWorker / UsbDmaWorker / CmdDmaWorker、IrscDriverAo 的
 *     action 全部为模板可见 / hpp 内联实现，本 TU 承载将来非模板实现移
 *     入位（运行时常量表、线程 tramp 落地、诊断打印去宏化等）。
 *   - 保留本文件以固定"每模块一对 .hpp/.cpp"结构，并给链接器一个稳定的
 *     符号实例锚点（避免不同编译单元对同一头产生不同 vtable/comdat）。
 *
 * 与其他文件的关系
 *   - 仅 include sensor_irsc.hpp（从而带入 common.hpp 的 Sig / DemoPal /
 *     g_pal / PoolT / Rt 等词汇）；不直接 include 其他业务文件。
 *   - 链接时被 isp_pipeline_demo 显式列出，保证每个 .hpp 都有对应 .cpp
 *     TU 出现在最终镜像中。
 *   - 依赖：sensor_irsc.hpp 暴露的全部 IRSC 产帧 / 驱动命令 / worker 分层
 *     API；语义不增不减。
 *
 * 文字图（与 hpp 视角一致，标注 TU 角色）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │  (hpp：本TU是    [video_stream]──kPicPacked──▶                  │
 * │   它的实现TU)      [output_itf]──kFrameEof──▶                   │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */

#include "sensor_irsc.hpp"
