/**
 * @file main.cpp
 * @brief Serial OTA demo -- showcasing newosp component integration.
 *
 * Architecture (loopback for demo):
 *   Host sends raw bytes -> Device parser -> DeviceHandler -> response bytes
 *   Device response bytes -> Host parser -> OtaHost::OnResponse
 *
 * newosp components used (12):
 *   - osp::StateMachine     -- Device OTA state machine + frame parser HSM
 *   - osp::BehaviorTree     -- Host upgrade flow (Sequence of actions)
 *   - osp::DebugShell       -- Telnet debug commands (OSP_SHELL_CMD)
 *   - osp::TimerScheduler   -- Periodic OTA tick + timeout monitoring
 *   - osp::AsyncBus         -- Message bus for OTA event notifications
 *   - osp::WorkerPool       -- Background event processing (dispatcher + workers)
 *   - osp::SpscRingbuffer   -- Simulated UART FIFO channels (host <-> device)
 *   - osp::FixedString      -- Stack-allocated status strings
 *   - osp::FixedVector      -- Stack-allocated firmware buffer
 *   - osp::expected         -- Error handling without exceptions
 *   - osp::ScopeGuard       -- RAII cleanup for resources
 *   - osp::log              -- Structured logging
 */

#include "device.hpp"
#include "host.hpp"
#include "ota_bridge.hpp"
#include "parser.hpp"
#include "protocol.hpp"

#include "osp/bus.hpp"
#include "osp/log.hpp"
#include "osp/shell.hpp"
#include "osp/spsc_ringbuffer.hpp"
#include "osp/timer.hpp"
#include "osp/vocabulary.hpp"
#include "osp/worker_pool.hpp"

#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <thread>
#include <variant>

// ============================================================================
// Configuration
// ============================================================================

static constexpr uint32_t kFirmwareSize = 4096U;
static constexpr uint32_t kChunkSize = 128U;
static constexpr uint32_t kFlashStartAddr = 0x0000U;
static constexpr uint16_t kShellPort = 5090U;
static constexpr uint32_t kOtaTickMs = 5U;          // BT tick interval
static constexpr uint32_t kTimeoutCheckMs = 500U;   // Timeout monitor interval
static constexpr uint32_t kMaxOtaTimeMs = 30000U;   // Max OTA duration
static constexpr uint32_t kWorkerNum = 2U;          // WorkerPool worker threads
static constexpr uint32_t kWorkerQueueDepth = 64U;  // Per-worker SPSC queue depth
static constexpr size_t kUartFifoSize = 512U;       // Simulated UART FIFO depth
static constexpr uint32_t kDropRate = 5U;           // ~5% data-chunk corruption rate

// ============================================================================
// Bus Message Types (osp::AsyncBus)
// ============================================================================

/// OTA progress notification (published by host tick timer).
struct OtaProgressMsg {
  uint32_t bytes_sent;
  uint32_t total_size;
  osp::NodeStatus status;
};

/// OTA state change notification (published on device state transitions).
struct OtaStateChangeMsg {
  osp::FixedString<32> old_state;
  osp::FixedString<32> new_state;
};

/// OTA completion notification.
struct OtaCompleteMsg {
  bool success;
  uint16_t fw_crc;
  uint16_t flash_crc;
  uint32_t elapsed_ms;
  uint32_t tick_count;
  uint32_t retries;
  uint32_t drops;
};

using OtaPayload = std::variant<OtaProgressMsg, OtaStateChangeMsg, OtaCompleteMsg>;
using OtaBus = osp::AsyncBus<OtaPayload>;
using OtaWorkerPool = osp::WorkerPool<OtaPayload>;

// ============================================================================
// Global State (for shell command access)
// ============================================================================

static ota::FrameParser g_host_parser;
static ota::FrameParser g_device_parser;
static ota::DeviceHandler<> g_device;
static ota::OtaHost* g_host = nullptr;
static OtaWorkerPool* g_pool = nullptr;
static std::atomic<bool> g_ota_running{false};
static std::atomic<uint32_t> g_tick_count{0};
static osp::FixedString<32> g_last_device_state{"Idle"};

// --- coact bridge state ---------------------------------------------------
// The parser callbacks on the main thread hand parsed host frames to coact;
// the coact Dispatcher thread owns OtaHost and drives its BehaviorTree tick.
static coact::Runtime<coact::DefaultConfig, coact::pal::Posix>* g_rt = nullptr;
static coact::EventPool<sizeof(ota_bridge::FrameEvent), 32U>* g_frame_pool = nullptr;
static std::atomic<bool> g_ota_done{false};


// Simulated UART FIFO channels (SpscRingbuffer)
static osp::SpscRingbuffer<uint8_t, kUartFifoSize> g_host_to_dev_fifo;
static osp::SpscRingbuffer<uint8_t, kUartFifoSize> g_dev_to_host_fifo;

// Channel corruption state (simple LCG PRNG for deterministic testing)
static uint32_t g_corrupt_rng = 12345U;
static uint32_t g_corrupt_count = 0U;

static uint32_t NextCorruptRng() noexcept {
  g_corrupt_rng = g_corrupt_rng * 1103515245U + 12345U;
  return (g_corrupt_rng >> 16) & 0x7FFFU;
}

// ============================================================================
// UART FIFO Loopback (SpscRingbuffer-based)
// ============================================================================

/// Host TX -> push to FIFO (simulates UART TX with channel noise).
/// Only OTA_DATA frames (cmd_class=0x04, cmd=0x02) are subject to
/// random byte corruption, causing CRC errors at the device parser.
static void HostSendToDevice(const uint8_t* data, uint32_t len, void* /*ctx*/) {
  // Corrupt only OTA_DATA frames: frame[3]=cmd_class, frame[4]=cmd
  if (kDropRate > 0U && len > 5U && data[3] == static_cast<uint8_t>(ota::CmdClass::kOta) &&
      data[4] == ota::ota_cmd::kData) {
    if (NextCorruptRng() % 100U < kDropRate) {
      // Copy frame and flip one bit in the payload area to trigger CRC error
      uint8_t corrupt_buf[ota::kMaxPayloadLen + ota::kFrameOverhead + 8U];
      uint32_t copy_len = len;
      if (copy_len > sizeof(corrupt_buf)) {
        copy_len = static_cast<uint32_t>(sizeof(corrupt_buf));
      }
      std::memcpy(corrupt_buf, data, copy_len);
      uint32_t pos = 3U + (NextCorruptRng() % (copy_len - 4U));
      corrupt_buf[pos] ^= 0x01U;
      ++g_corrupt_count;
      g_host_to_dev_fifo.PushBatch(corrupt_buf, static_cast<size_t>(copy_len));
      return;
    }
  }
  g_host_to_dev_fifo.PushBatch(data, static_cast<size_t>(len));
}

/// Device TX -> push to FIFO (simulates UART TX FIFO).
static void DeviceSendToHost(const uint8_t* data, uint32_t len, void* /*ctx*/) {
  g_dev_to_host_fifo.PushBatch(data, static_cast<size_t>(len));
}

/// Drain both FIFO channels into their respective parsers.
/// Called once per main-loop iteration (simulates UART RX interrupt).
static void DrainUartFifos() noexcept {
  // Host -> Device direction
  uint8_t buf[256];
  while (!g_host_to_dev_fifo.IsEmpty()) {
    size_t n = g_host_to_dev_fifo.PopBatch(buf, sizeof(buf));
    if (n == 0)
      break;
    g_device_parser.PutData(buf, static_cast<uint32_t>(n));
  }

  // Device -> Host direction
  while (!g_dev_to_host_fifo.IsEmpty()) {
    size_t n = g_dev_to_host_fifo.PopBatch(buf, sizeof(buf));
    if (n == 0)
      break;
    g_host_parser.PutData(buf, static_cast<uint32_t>(n));
  }
}

// ============================================================================
// Frame Callbacks
// ============================================================================

static void DeviceFrameCallback(const ota::Frame& frame, void* /*ctx*/) {
  // Track device state changes via WorkerPool (kHigh priority)
  osp::FixedString<32> old_state(osp::TruncateToCapacity, g_device.GetOtaStateName());

  g_device.ProcessFrame(frame);

  osp::FixedString<32> new_state(osp::TruncateToCapacity, g_device.GetOtaStateName());
  if (old_state != new_state) {
    g_last_device_state = new_state;
    if (g_pool != nullptr) {
      g_pool->Submit(OtaStateChangeMsg{old_state, new_state}, osp::MessagePriority::kHigh);
    }
  }
}

static void HostFrameCallback(const ota::Frame& frame, void* /*ctx*/) {
  // Hand the parsed host-targeted frame to the coact Dispatcher thread, which
  // owns OtaHost. A pool-allocated FrameEvent carries the frame payload across
  // the thread boundary (zero aliasing: the block is copied, not borrowed).
  if (g_frame_pool != nullptr && g_rt != nullptr) {
    coact::Event* e = g_frame_pool->alloc(ota_bridge::kSigHostFrame);
    if (e != nullptr) {
      auto* fe = reinterpret_cast<ota_bridge::FrameEvent*>(e);
      fe->frame = frame;
      coact::EventQos qos{false, false};
      g_rt->coordinator().submit_from_task(ota_bridge::kOtaBridgeTargetId, e, qos);
    }
  }
}

// ============================================================================
// Timer Callbacks (osp::TimerScheduler)
// ============================================================================

/// Periodic progress reporter (timer-driven, does NOT tick the BT).
/// HostContext is owned by the coact Dispatcher thread, so this timer thread
/// must NOT read it directly; progress is logged by the coact Ao per tick.
static void ProgressReportCallback(void* /*ctx*/) {
  if (!g_ota_running.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_pool != nullptr) {
    g_pool->Submit(OtaProgressMsg{0U, kFirmwareSize, osp::NodeStatus::kRunning},
                   osp::MessagePriority::kLow);
  }
}

/// Periodic timeout monitor -- checks if OTA has exceeded max duration.
static void TimeoutCheckCallback(void* ctx) {
  auto* start_time = static_cast<std::chrono::steady_clock::time_point*>(ctx);
  if (!g_ota_running.load(std::memory_order_relaxed))
    return;

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *start_time);

  if (static_cast<uint32_t>(elapsed.count()) > kMaxOtaTimeMs) {
    OSP_LOG_ERROR("OTA_MAIN", "OTA timeout after %u ms", static_cast<uint32_t>(elapsed.count()));
    g_ota_running.store(false, std::memory_order_relaxed);
  }
}

// ============================================================================
// WorkerPool Handlers (free functions, -fno-rtti compatible)
// ============================================================================

/// Worker handler: log OTA state transitions.
static void HandleStateChange(const OtaStateChangeMsg& msg, const osp::MessageHeader& /*hdr*/) {
  OSP_LOG_INFO("OTA_POOL", "State: %s -> %s", msg.old_state.c_str(), msg.new_state.c_str());
}

/// Worker handler: log OTA progress.
static void HandleProgress(const OtaProgressMsg& msg, const osp::MessageHeader& /*hdr*/) {
  uint32_t pct = (msg.total_size > 0U) ? (msg.bytes_sent * 100U / msg.total_size) : 0U;
  OSP_LOG_INFO("OTA_POOL", "Progress: %u/%u (%u%%) status=%s", msg.bytes_sent, msg.total_size, pct,
               osp::NodeStatusToString(msg.status));
}

/// Worker handler: log OTA completion.
static void HandleComplete(const OtaCompleteMsg& msg, const osp::MessageHeader& /*hdr*/) {
  if (msg.success) {
    OSP_LOG_INFO("OTA_POOL",
                 "OTA OK: %u ms, %u ticks, CRC=0x%04X, "
                 "retries=%u drops=%u",
                 msg.elapsed_ms, msg.tick_count, msg.fw_crc, msg.retries, msg.drops);
  } else {
    OSP_LOG_ERROR("OTA_POOL",
                  "OTA FAILED: %u ms, %u ticks, "
                  "retries=%u drops=%u",
                  msg.elapsed_ms, msg.tick_count, msg.retries, msg.drops);
  }
}

// ============================================================================
// Shell Commands (osp::DebugShell)
// ============================================================================

static int cmd_ota_status(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== OTA Status ===\r\n");
  osp::DebugShell::Printf("Device SM:  %s\r\n", g_device.GetOtaStateName());

  const auto& dctx = g_device.GetContext();
  osp::DebugShell::Printf("  total:    %u bytes\r\n", dctx.total_size);
  osp::DebugShell::Printf("  addr:     0x%X\r\n", dctx.start_addr);
  osp::DebugShell::Printf("  received: %u bytes\r\n", dctx.received_size);
  osp::DebugShell::Printf("  exp_crc:  0x%04X\r\n", dctx.expected_crc);
  osp::DebugShell::Printf("  calc_crc: 0x%04X\r\n", dctx.calc_crc);

  if (g_host != nullptr) {
    osp::DebugShell::Printf("\r\nHost BT:    %s\r\n", osp::NodeStatusToString(g_host->GetStatus()));
    osp::DebugShell::Printf("  progress: %.1f%%\r\n", static_cast<double>(g_host->GetProgress()) * 100.0);
    osp::DebugShell::Printf("  sent:     %u bytes\r\n", g_host->GetBytesSent());
    osp::DebugShell::Printf("  ticks:    %u\r\n", g_tick_count.load(std::memory_order_relaxed));
  }
  return 0;
}
OSP_SHELL_CMD(cmd_ota_status, "Show OTA state machine and transfer progress");

static int cmd_serial_stats(int /*argc*/, char* /*argv*/[]) {
  const auto& hs = g_host_parser.GetStats();
  const auto& ds = g_device_parser.GetStats();

  osp::DebugShell::Printf("=== Host Parser ===\r\n");
  osp::DebugShell::Printf("  state:    %s\r\n", g_host_parser.CurrentStateName());
  osp::DebugShell::Printf("  bytes_rx: %u\r\n", hs.bytes_received);
  osp::DebugShell::Printf("  frames:   %u\r\n", hs.frames_received);
  osp::DebugShell::Printf("  sync_err: %u  crc_err: %u  tail_err: %u\r\n", hs.sync_errors, hs.crc_errors,
                          hs.tail_errors);

  osp::DebugShell::Printf("\r\n=== Device Parser ===\r\n");
  osp::DebugShell::Printf("  state:    %s\r\n", g_device_parser.CurrentStateName());
  osp::DebugShell::Printf("  bytes_rx: %u\r\n", ds.bytes_received);
  osp::DebugShell::Printf("  frames:   %u\r\n", ds.frames_received);
  osp::DebugShell::Printf("  sync_err: %u  crc_err: %u  tail_err: %u\r\n", ds.sync_errors, ds.crc_errors,
                          ds.tail_errors);
  return 0;
}
OSP_SHELL_CMD(cmd_serial_stats, "Show host/device serial parser statistics");

static int cmd_bus_stats(int /*argc*/, char* /*argv*/[]) {
  auto stats = OtaBus::Instance().GetStatistics();
  osp::DebugShell::Printf("=== Bus Statistics ===\r\n");
  osp::DebugShell::Printf("  published: %lu\r\n", static_cast<unsigned long>(stats.messages_published));
  osp::DebugShell::Printf("  processed: %lu\r\n", static_cast<unsigned long>(stats.messages_processed));
  osp::DebugShell::Printf("  dropped:   %lu\r\n", static_cast<unsigned long>(stats.messages_dropped));
  osp::DebugShell::Printf("  depth:     %u / %u (%u%%)\r\n", OtaBus::Instance().Depth(), OtaBus::kQueueDepth,
                          OtaBus::Instance().UtilizationPercent());
  return 0;
}
OSP_SHELL_CMD(cmd_bus_stats, "Show message bus statistics");

static int cmd_flash_dump(int argc, char* argv[]) {
  if (argc < 2) {
    osp::DebugShell::Printf("Usage: cmd_flash_dump <addr> [len]\r\n");
    return -1;
  }
  uint32_t addr = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0));
  uint32_t len = (argc >= 3) ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0)) : 64U;

  const auto& flash = g_device.GetFlash();
  if (addr + len > flash.Size()) {
    osp::DebugShell::Printf("Error: out of range (flash=%u)\r\n", flash.Size());
    return -1;
  }

  uint8_t buf[256];
  uint32_t chunk = (len > sizeof(buf)) ? static_cast<uint32_t>(sizeof(buf)) : len;
  if (!const_cast<ota::FlashSim<>&>(flash).Read(addr, buf, chunk)) {
    osp::DebugShell::Printf("Error: read failed\r\n");
    return -1;
  }

  for (uint32_t i = 0; i < chunk; ++i) {
    if (i % 16U == 0) {
      osp::DebugShell::Printf("  %08X: ", addr + i);
    }
    osp::DebugShell::Printf("%02X ", buf[i]);
    if ((i + 1U) % 16U == 0 || i + 1U == chunk) {
      osp::DebugShell::Printf("\r\n");
    }
  }
  return 0;
}
OSP_SHELL_CMD(cmd_flash_dump, "Hex dump flash: cmd_flash_dump <addr> [len]");

static int cmd_flash_crc(int argc, char* argv[]) {
  if (argc < 3) {
    osp::DebugShell::Printf("Usage: cmd_flash_crc <addr> <len>\r\n");
    return -1;
  }
  uint32_t addr = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0));
  uint32_t len = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));

  uint16_t crc = g_device.GetFlash().CalcCrc(addr, len);
  osp::DebugShell::Printf("CRC16(0x%X, %u) = 0x%04X\r\n", addr, len, crc);
  return 0;
}
OSP_SHELL_CMD(cmd_flash_crc, "Calc flash CRC: cmd_flash_crc <addr> <len>");

static int cmd_timer_info(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== Timer Info ===\r\n");
  osp::DebugShell::Printf("  OTA tick interval:  %u ms\r\n", kOtaTickMs);
  osp::DebugShell::Printf("  Timeout check:      %u ms\r\n", kTimeoutCheckMs);
  osp::DebugShell::Printf("  Max OTA duration:   %u ms\r\n", kMaxOtaTimeMs);
  osp::DebugShell::Printf("  OTA running:        %s\r\n", g_ota_running.load() ? "yes" : "no");
  return 0;
}
OSP_SHELL_CMD(cmd_timer_info, "Show timer scheduler configuration");

static int cmd_pool_stats(int /*argc*/, char* /*argv*/[]) {
  if (g_pool == nullptr) {
    osp::DebugShell::Printf("WorkerPool not initialized\r\n");
    return -1;
  }
  auto ps = g_pool->GetStats();
  osp::DebugShell::Printf("=== WorkerPool Statistics ===\r\n");
  osp::DebugShell::Printf("  workers:    %u\r\n", g_pool->WorkerCount());
  osp::DebugShell::Printf("  running:    %s\r\n", g_pool->IsRunning() ? "yes" : "no");
  osp::DebugShell::Printf("  paused:     %s\r\n", g_pool->IsPaused() ? "yes" : "no");
  osp::DebugShell::Printf("  dispatched: %lu\r\n", static_cast<unsigned long>(ps.dispatched));
  osp::DebugShell::Printf("  processed:  %lu\r\n", static_cast<unsigned long>(ps.processed));
  osp::DebugShell::Printf("  queue_full: %lu\r\n", static_cast<unsigned long>(ps.worker_queue_full));
  return 0;
}
OSP_SHELL_CMD(cmd_pool_stats, "Show WorkerPool dispatcher/worker statistics");

static int cmd_uart_fifo(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== UART FIFO (SpscRingbuffer) ===\r\n");
  osp::DebugShell::Printf("  host->dev:  size=%zu / %zu\r\n", static_cast<size_t>(g_host_to_dev_fifo.Size()),
                          g_host_to_dev_fifo.Capacity());
  osp::DebugShell::Printf("  dev->host:  size=%zu / %zu\r\n", static_cast<size_t>(g_dev_to_host_fifo.Size()),
                          g_dev_to_host_fifo.Capacity());
  return 0;
}
OSP_SHELL_CMD(cmd_uart_fifo, "Show simulated UART FIFO status");

static int cmd_retransmit(int /*argc*/, char* /*argv*/[]) {
  osp::DebugShell::Printf("=== Retransmission Stats ===\r\n");
  osp::DebugShell::Printf("  drop_rate:     %u%%\r\n", kDropRate);
  osp::DebugShell::Printf("  corrupted:     %u frames\r\n", g_corrupt_count);
  if (g_host != nullptr) {
    osp::DebugShell::Printf("  retries:       %u\r\n", g_host->GetTotalRetries());
    osp::DebugShell::Printf("  drops:         %u\r\n", g_host->GetTotalDrops());
    osp::DebugShell::Printf("  max_per_chunk: %u\r\n", ota::HostContext::kMaxChunkRetries);
  }
  return 0;
}
OSP_SHELL_CMD(cmd_retransmit, "Show channel corruption & retransmission stats");

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  bool use_console = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0)
      use_console = true;
  }

  OSP_LOG_INFO("OTA_MAIN", "=== Serial OTA Demo ===");
  OSP_LOG_INFO("OTA_MAIN",
               "Components: StateMachine + BehaviorTree + "
               "DebugShell + TimerScheduler + AsyncBus + WorkerPool + "
               "SpscRingbuffer + vocabulary");
  OSP_LOG_INFO("OTA_MAIN", "Firmware: %u bytes, chunk: %u, addr: 0x%X", kFirmwareSize, kChunkSize, kFlashStartAddr);

  // --- Generate fake firmware using FixedVector ------------------------------
  osp::FixedVector<uint8_t, kFirmwareSize> firmware;
  for (uint32_t i = 0; i < kFirmwareSize; ++i) {
    firmware.push_back(static_cast<uint8_t>(i & 0xFFU));
  }
  OSP_LOG_INFO("OTA_MAIN", "Firmware generated: %u bytes (FixedVector cap=%u)", firmware.size(), firmware.capacity());

  // --- Setup bus + WorkerPool (dispatcher + 2 workers) -----------------------
  OtaBus::Instance().Reset();

  osp::WorkerPoolConfig pool_cfg;
  pool_cfg.name = "ota_pool";
  pool_cfg.worker_num = kWorkerNum;
  OtaWorkerPool pool(pool_cfg);
  pool.RegisterHandler<OtaStateChangeMsg>(&HandleStateChange);
  pool.RegisterHandler<OtaProgressMsg>(&HandleProgress);
  pool.RegisterHandler<OtaCompleteMsg>(&HandleComplete);
  g_pool = &pool;

  // RAII cleanup for g_pool pointer
  OSP_SCOPE_EXIT(g_pool = nullptr);

  // --- Setup parsers (HSM-based) ---------------------------------------------
  g_device_parser.SetCallback(DeviceFrameCallback);
  g_device_parser.Start();

  g_host_parser.SetCallback(HostFrameCallback);
  g_host_parser.Start();

  // --- Setup device (StateMachine-based) -------------------------------------
  g_device.SetResponseCallback(DeviceSendToHost, nullptr);

  // --- Setup host (BehaviorTree-based) ---------------------------------------
  ota::OtaHost host(firmware.data(), firmware.size(), kFlashStartAddr, kChunkSize);
  host.SetSendCallback(HostSendToDevice, nullptr);
  g_host = &host;

  // RAII cleanup for g_host pointer
  OSP_SCOPE_EXIT(g_host = nullptr);

  // --- coact bridge: Runtime + Dispatcher thread drive OtaHost --------------
  // The bridge Ao owns OtaHost; both host.Tick() and host.OnResponse() run on
  // the coact Dispatcher thread so HostContext is never accessed concurrently.
  coact::pal::Posix pal;

  // The frame pool is shared between the main thread (alloc) and the coact
  // Dispatcher thread (batched reclaim). On SMP the POSIX no-op CriticalSection
  // would let their `next`-field writes race (see ReclaimBatcher), so inject a
  // real spinlock CS here. Single-core RT-Thread pools use irq-mask instead.
  coact::SpinCriticalSection frame_pool_cs;

  coact::EventPool<sizeof(ota_bridge::FrameEvent), 32U> frame_pool;
  alignas(16) unsigned char frame_storage[sizeof(ota_bridge::FrameEvent) * 32U + 16U];
  frame_pool.init(frame_storage, sizeof(frame_storage),
                  coact::make_spin_critical_section(frame_pool_cs));
  g_frame_pool = &frame_pool;
  OSP_SCOPE_EXIT(g_frame_pool = nullptr);

  coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
  g_rt = &rt;
  OSP_SCOPE_EXIT(g_rt = nullptr);

  ota_bridge::BridgeAo bridge(
      ota_bridge::kStates,
      static_cast<uint16_t>(sizeof(ota_bridge::kStates) / sizeof(ota_bridge::kStates[0])),
      ota_bridge::kTransitions,
      static_cast<uint16_t>(sizeof(ota_bridge::kTransitions) / sizeof(ota_bridge::kTransitions[0])),
      static_cast<int8_t>(0), /*initial_state=*/static_cast<uint8_t>(1U));
  bridge.context().host = &host;
  bridge.context().done = &g_ota_done;

  coact::Event init_e;
  init_e.signal = 0U;
  init_e.pool_id = 0U;
  init_e.ref_ctr = 0U;
  bridge.init(init_e);

  if (!rt.bind(&bridge)) {
    OSP_LOG_ERROR("OTA_MAIN", "coact bridge bind failed");
    return 1;
  }
  rt.initialize();
  rt.start();
  OSP_LOG_INFO("OTA_MAIN", "coact: bridge Ao drives OtaHost on Dispatcher thread");

  // --- Start shell (osp::ConsoleShell or osp::DebugShell) --------------------
  osp::ConsoleShell console_shell;
  osp::DebugShell::Config tcp_cfg;
  tcp_cfg.port = kShellPort;
  osp::DebugShell tcp_shell(tcp_cfg);

  bool shell_ok = false;
  if (use_console) {
    auto r = console_shell.Start();
    shell_ok = r.has_value();
    if (shell_ok) {
      OSP_LOG_INFO("OTA_MAIN", "Console shell started (stdin/stdout)");
    }
  } else {
    auto r = tcp_shell.Start();
    shell_ok = r.has_value();
    if (shell_ok) {
      OSP_LOG_INFO("OTA_MAIN", "Debug shell: telnet localhost %u", kShellPort);
    }
  }
  if (shell_ok) {
    OSP_LOG_INFO("OTA_MAIN",
                 "  Commands: cmd_ota_status, cmd_serial_stats, "
                 "cmd_bus_stats, cmd_pool_stats, cmd_uart_fifo, cmd_retransmit, "
                 "cmd_flash_dump, cmd_flash_crc, cmd_timer_info");
  }

  // --- Start WorkerPool (must be before OTA start, subscribers need to be active) ---
  pool.Start();
  OSP_LOG_INFO("OTA_MAIN", "WorkerPool: %u workers, queue=%u", kWorkerNum, kWorkerQueueDepth);

  // --- Start OTA upgrade (BehaviorTree) --------------------------------------
  OSP_LOG_INFO("OTA_MAIN", "Starting OTA upgrade...");
  if (!host.Start()) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to start OTA");
    return 1;
  }

  g_ota_running.store(true, std::memory_order_relaxed);
  auto ota_start_time = std::chrono::steady_clock::now();

  // --- Setup timer scheduler (osp::TimerScheduler) ---------------------------
  // Timer handles progress reporting + timeout monitoring (background thread).
  // BT ticking is done in the main loop for deterministic loopback execution.
  osp::TimerScheduler<4> timer;

  auto progress_result = timer.Add(kOtaTickMs * 50U, ProgressReportCallback);
  if (!progress_result) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to add progress timer");
    return 1;
  }
  OSP_LOG_INFO("OTA_MAIN", "Timer: progress report every %u ms", kOtaTickMs * 50U);

  auto timeout_result = timer.Add(kTimeoutCheckMs, TimeoutCheckCallback, &ota_start_time);
  if (!timeout_result) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to add timeout timer");
    return 1;
  }
  OSP_LOG_INFO("OTA_MAIN", "Timer: timeout check every %u ms (max %u ms)", kTimeoutCheckMs, kMaxOtaTimeMs);

  auto timer_start = timer.Start();
  if (!timer_start) {
    OSP_LOG_ERROR("OTA_MAIN", "Failed to start timer scheduler");
    return 1;
  }

  // RAII cleanup for timer
  OSP_SCOPE_EXIT(timer.Stop());

  // --- Main loop: pump coact host ticks + drain UART FIFOs ------------------
  // host.Tick() runs on the coact Dispatcher thread (owned by the bridge Ao);
  // the main thread only feeds the FIFOs (device side) and submits a tick.
  // The Ao sets g_ota_done when the BehaviorTree reaches SUCCESS/FAILURE.
  OSP_LOG_INFO("OTA_MAIN", "OTA running...");
  constexpr uint32_t kMaxTicks = 50000U;

  while (g_ota_running.load(std::memory_order_relaxed) && !g_ota_done.load(std::memory_order_acquire) &&
         g_tick_count.load(std::memory_order_relaxed) < kMaxTicks) {
    // Drain UART FIFOs: host->dev bytes feed device parser,
    // dev->host bytes feed host parser (simulates RX interrupt).
    DrainUartFifos();

    // Ask the coact Dispatcher to run one host BehaviorTree step.
    if (g_rt != nullptr) {
      coact::Event* te = g_frame_pool->alloc(ota_bridge::kSigHostTick);
      if (te != nullptr) {
        coact::EventQos qos{false, false};
        g_rt->coordinator().submit_from_task(ota_bridge::kOtaBridgeTargetId, te, qos);
      }
    }

    g_tick_count.fetch_add(1, std::memory_order_relaxed);

    // Pace ticks (kOtaTickMs cadence) so the Dispatcher can drain frame
    // events in between; a hot flood would starve them out and drop ACKs.
    std::this_thread::sleep_for(std::chrono::milliseconds(kOtaTickMs));
  }

  // Stop the coact Dispatcher so all remaining host reads below are single-thread.
  if (g_rt != nullptr) {
    g_rt->stop();
  }

  // Stop timer before reading final state
  timer.Stop();

  // --- Results ---------------------------------------------------------------
  auto ota_end_time = std::chrono::steady_clock::now();
  uint32_t elapsed_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(ota_end_time - ota_start_time).count());
  uint32_t ticks = g_tick_count.load(std::memory_order_relaxed);

  OSP_LOG_INFO("OTA_MAIN", "");
  OSP_LOG_INFO("OTA_MAIN", "========== Results ==========");
  OSP_LOG_INFO("OTA_MAIN", "Elapsed:      %u ms", elapsed_ms);
  OSP_LOG_INFO("OTA_MAIN", "BT ticks:     %u", ticks);
  OSP_LOG_INFO("OTA_MAIN", "Host status:  %s", osp::NodeStatusToString(host.GetStatus()));
  OSP_LOG_INFO("OTA_MAIN", "Bytes sent:   %u / %u", host.GetBytesSent(), kFirmwareSize);
  OSP_LOG_INFO("OTA_MAIN", "Device state: %s", g_device.GetOtaStateName());

  const auto& hs = g_host_parser.GetStats();
  const auto& ds = g_device_parser.GetStats();
  OSP_LOG_INFO("OTA_MAIN", "Host parser:  %u frames, %u bytes", hs.frames_received, hs.bytes_received);
  OSP_LOG_INFO("OTA_MAIN", "Dev parser:   %u frames, %u bytes", ds.frames_received, ds.bytes_received);
  OSP_LOG_INFO("OTA_MAIN", "Dev CRC err:  %u (corrupted: %u)", ds.crc_errors, g_corrupt_count);

  // Retransmission statistics
  OSP_LOG_INFO("OTA_MAIN", "Retransmit:   retries=%u drops=%u (rate=%u%%)", host.GetTotalRetries(),
               host.GetTotalDrops(), kDropRate);

  // Bus statistics
  auto bus_stats = OtaBus::Instance().GetStatistics();
  OSP_LOG_INFO(
      "OTA_MAIN", "Bus msgs:     pub=%lu proc=%lu drop=%lu", static_cast<unsigned long>(bus_stats.messages_published),
      static_cast<unsigned long>(bus_stats.messages_processed), static_cast<unsigned long>(bus_stats.messages_dropped));

  // CRC verification
  bool ota_success = host.IsComplete();
  uint16_t fw_crc = 0;
  uint16_t flash_crc = 0;

  if (ota_success) {
    fw_crc = ota::CalcCrc16(firmware.data(), firmware.size());
    flash_crc = g_device.GetFlash().CalcCrc(kFlashStartAddr, kFirmwareSize);
    ota_success = (fw_crc == flash_crc);
    OSP_LOG_INFO("OTA_MAIN", "FW CRC=0x%04X  Flash CRC=0x%04X  %s", fw_crc, flash_crc,
                 ota_success ? "MATCH" : "MISMATCH");
    OSP_LOG_INFO("OTA_MAIN", "OTA upgrade completed successfully!");
  } else if (host.IsFailed()) {
    OSP_LOG_ERROR("OTA_MAIN", "OTA upgrade FAILED");
  } else {
    OSP_LOG_WARN("OTA_MAIN", "OTA timed out after %u ms", elapsed_ms);
  }

  // Publish completion event via WorkerPool (kHigh priority)
  pool.Submit(
      OtaCompleteMsg{ota_success, fw_crc, flash_crc, elapsed_ms, ticks, host.GetTotalRetries(), host.GetTotalDrops()},
      osp::MessagePriority::kHigh);

  // Drain all in-flight events before shutdown
  pool.FlushAndPause();

  // WorkerPool statistics
  auto pool_stats = pool.GetStats();
  OSP_LOG_INFO("OTA_MAIN", "Pool:         dispatched=%lu processed=%lu qfull=%lu",
               static_cast<unsigned long>(pool_stats.dispatched), static_cast<unsigned long>(pool_stats.processed),
               static_cast<unsigned long>(pool_stats.worker_queue_full));

  OSP_LOG_INFO("OTA_MAIN", "=============================");

  // --- Shell interaction (keep alive briefly) --------------------------------
  if (shell_ok) {
    if (use_console) {
      OSP_LOG_INFO("OTA_MAIN", "Console shell active -- waiting 3s for inspection");
    } else {
      OSP_LOG_INFO("OTA_MAIN", "Shell on port %u -- waiting 3s for inspection", kShellPort);
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }

  // --- Shutdown (reverse order of start) ------------------------------------
  pool.Shutdown();
  if (use_console) {
    console_shell.Stop();
  } else {
    tcp_shell.Stop();
  }
  OSP_LOG_INFO("OTA_MAIN", "Demo finished.");
  return 0;
}
