/*
 * ===========================================================================
 * recfg_session.cpp —— recfg_session.hpp 的实现 TU（重配事务/缓存治理）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 recfg_session.hpp 的实现 TU。SessionState 六相位 + 原子
 *     g_session 推进、IspPipelineAO 的 8 节点 init 序列状态表、RecfgOrchAo
 *     五步事务（precheck/snapshot/quiesce/apply/resume+commit）的弧 action
 *     与 RecfgStage 姿态表、AddrCache / PeriphRegCache / BypassGuard /
 *     DisplayMirror 的辅助结构，全部为 hpp 编译期定义，本 TU 承载将来非
 *     模板实现移入位（事务姿态表外置、寄存器缓存拆 TU、DmoQuiesce 步骤
 *     拆分、诊断打印去宏化等）。
 *   - 保留本文件以固定"每模块一对 .hpp/.cpp"结构，并给链接器一个稳定的
 *     符号实例锚点。
 *
 * 与其他文件的关系
 *   - 仅 include recfg_session.hpp（从而带入 common.hpp 的 SessionState/
 *     g_session/session_advance/Sig/DdrCtx/g_pal/PoolT/Rt 等词汇）；不直接
 *     include 其他业务文件。
 *   - 链接时被 isp_pipeline_demo 显式列出；本 TU 不提供任何外部可见符号，
 *     但保证 RecfgOrchAo / IspPipelineAO 的可实例化编译通路在最终镜像中闭合。
 *   - 依赖：recfg_session.hpp 暴露的全部重配事务 / ISP 管线初始化 / 寄存
 *     器缓存 / 镜像变换 API；语义不增不减。
 *
 * 文字图（与 hpp 视角一致，标注 TU 角色）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  (hpp：本TU是它的实现TU)                                         │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */

#include "recfg_session.hpp"

