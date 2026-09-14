/*
 * ===========================================================================
 * output_itf.cpp —— output_itf.hpp 的实现 TU（sink/WRAPE/WinHost/Orch）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 output_itf.hpp 的实现 TU。UsbSinkAo / MipiSinkAo 的 DDR 字
 *     节级校验 action、WrapeAo 的成帧与 HwStateBlackboard 读取、WinHostAo
 *     的观察者行为、OrchestratorAo 的 ready 收集弧，以及 HwStateBlackboard
 *     的四要素（ZoomBlock / StreamSelBlock / WrapeBlock）全部为 hpp 编译
 *     期定义，本 TU 承载将来非模板实现移入位（sink tag 表外置、WRAPE
 *     geometry 计算拆 TU、诊断打印去宏化等）。
 *   - 保留本文件以固定"每模块一对 .hpp/.cpp"结构，并给链接器一个稳定的
 *     符号实例锚点。
 *
 * 与其他文件的关系
 *   - 仅 include output_itf.hpp（从而带入 common.hpp 与 isp_chain.hpp 的
 *     Sig / DdrCtx / HSM 表 / g_pal / PoolT / Rt 等词汇）；不直接 include
 *     其他业务文件。
 *   - 链接时被 isp_pipeline_demo 显式列出；本 TU 不提供任何外部可见符号，
 *     但保证 sink/WRAPE/Orchestrator AO 的可实例化编译通路在最终镜像中闭合。
 *   - 依赖：output_itf.hpp 暴露的全部输出接口 / WRAPE / WinHost / 编排
 *     API；语义不增不减。
 *
 * 文字图（与 hpp 视角一致，标注 TU 角色）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                              (hpp：本TU是它的实现TU)             │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */

#include "output_itf.hpp"

