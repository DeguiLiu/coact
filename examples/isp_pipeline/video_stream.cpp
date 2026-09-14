/*
 * ===========================================================================
 * video_stream.cpp —— video_stream.hpp 的实现 TU（FSM + 打包 + quiesce）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 video_stream.hpp 的实现 TU。VideoFsmAo（含 PIC/TEMP 两套
 *     节点 init/deinit 链与子状态）、VideoPackAo（PIC_ACTIVE/TEMP_ACTIVE
 *     合并 AO + SoutDmaWorker 写回通道）以及 EventQuiesce/PollQuiesce/
 *     QuiescePolicy 三种 quiesce 策略的乘积态表与 CRTP 骨架全部为 hpp 编译
 *     期定义，本 TU 承载将来非模板实现移入位（节点名表外置、HSM 表拆分
 *     到独立 TU、诊断打印去宏化等）。
 *   - 保留本文件以固定"每模块一对 .hpp/.cpp"结构，并给链接器一个稳定的
 *     符号实例锚点。
 *
 * 与其他文件的关系
 *   - 仅 include video_stream.hpp（从而带入 common.hpp 与 isp_chain.hpp 的
 *     Sig / DdrCtx / g_pal / PoolT / Rt 等词汇）；不直接 include 其他业务文件。
 *   - 链接时被 isp_pipeline_demo 显式列出；本 TU 不提供任何外部可见符号，
 *     但保证 FSM AO 与合并 pack AO 的可实例化编译通路在最终镜像中闭合。
 *   - 依赖：video_stream.hpp 暴露的全部视频 FSM / 打包 / quiesce API；
 *     语义不增不减。
 *
 * 文字图（与 hpp 视角一致，标注 TU 角色）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                              (hpp：本TU是它的实现TU)             │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */

#include "video_stream.hpp"

