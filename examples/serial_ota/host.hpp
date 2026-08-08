/**
 * @file host.hpp
 * @brief Host-side OTA upgrade flow driven by osp::BehaviorTree.
 *
 * BehaviorTree structure:
 *   Sequence: ota_upgrade
 *     +- Action: send_start   (send once, then wait for ACK)
 *     +- Action: send_chunks  (send one chunk per tick, kRunning until done)
 *     +- Action: send_end     (send once, then wait for ACK)
 *     +- Action: send_verify  (send once, then wait for verify response)
 *
 * Each action is a mini state machine: send on first call, then poll for
 * response on subsequent calls. This is necessary because BT Sequence
 * re-ticks all children from the start on each Tick().
 */

#ifndef SERIAL_OTA_HOST_HPP_
#define SERIAL_OTA_HOST_HPP_

#include "protocol.hpp"

#include "osp/bt.hpp"
#include "osp/log.hpp"

#include <cstdint>
#include <cstring>

namespace ota {

// ============================================================================
// Send Callback
// ============================================================================

using SendCallback = void (*)(const uint8_t* data, uint32_t len, void* ctx);

// ============================================================================
// Host Context
// ============================================================================

/// Phase tracking for actions that send-then-wait.
enum class ActionPhase : uint8_t {
  kSend = 0,  ///< Need to send the command
  kWait,      ///< Waiting for response
  kDone       ///< Action completed
};

struct HostContext {
  // Firmware image
  const uint8_t* firmware_data = nullptr;
  uint32_t firmware_size = 0;
  uint32_t start_addr = 0;
  uint16_t expected_crc = 0;

  // Transfer params
  uint32_t chunk_size = 128;
  uint32_t bytes_sent = 0;
  uint32_t current_offset = 0;

  // Response tracking
  bool ack_received = false;
  uint8_t last_status = 0;
  uint32_t last_received_size = 0;
  uint16_t verify_crc = 0;
  bool verify_received = false;
  bool verify_ok = false;

  // Retransmission
  static constexpr uint32_t kMaxChunkRetries = 3U;
  uint32_t chunk_retry_count = 0;
  uint32_t total_retries = 0;
  uint32_t total_drops = 0;
  bool data_nak_received = false;

  // Action phases (to avoid re-sending on BT re-tick)
  ActionPhase start_phase = ActionPhase::kSend;
  ActionPhase end_phase = ActionPhase::kSend;
  ActionPhase verify_phase = ActionPhase::kSend;

  // Send callback
  SendCallback send_fn = nullptr;
  void* send_ctx = nullptr;
};

// ============================================================================
// BehaviorTree Action Functions
// ============================================================================

namespace host_detail {

/// Send OTA_START, then wait for ACK.
inline osp::NodeStatus SendStart(HostContext& ctx) {
  if (ctx.send_fn == nullptr)
    return osp::NodeStatus::kFailure;

  if (ctx.start_phase == ActionPhase::kDone) {
    return osp::NodeStatus::kSuccess;
  }

  if (ctx.start_phase == ActionPhase::kSend) {
    OtaStartReq req;
    req.total_size = ctx.firmware_size;
    req.start_addr = ctx.start_addr;
    req.expected_crc = ctx.expected_crc;

    uint8_t frame_buf[64];
    uint32_t n = BuildFrame(frame_buf, sizeof(frame_buf), static_cast<uint8_t>(CmdClass::kOta), ota_cmd::kStart, &req,
                            sizeof(req));
    if (n == 0)
      return osp::NodeStatus::kFailure;

    // Reset state BEFORE send -- loopback may set ack_received synchronously
    ctx.ack_received = false;
    ctx.bytes_sent = 0;
    ctx.current_offset = 0;
    ctx.start_phase = ActionPhase::kWait;
    ctx.send_fn(frame_buf, n, ctx.send_ctx);

    OSP_LOG_INFO("OTA_HOST", "Sent START: addr=0x%X size=%u crc=0x%04X", ctx.start_addr, ctx.firmware_size,
                 ctx.expected_crc);
    return osp::NodeStatus::kRunning;
  }

  // kWait phase
  if (!ctx.ack_received)
    return osp::NodeStatus::kRunning;

  if (ctx.last_status == static_cast<uint8_t>(Status::kSuccess)) {
    ctx.start_phase = ActionPhase::kDone;
    OSP_LOG_INFO("OTA_HOST", "START ACK received");
    return osp::NodeStatus::kSuccess;
  }
  OSP_LOG_ERROR("OTA_HOST", "START ACK failed: status=%u", ctx.last_status);
  return osp::NodeStatus::kFailure;
}

/// Send firmware chunks one per tick, with NAK-based retransmission.
inline osp::NodeStatus SendChunks(HostContext& ctx) {
  if (ctx.send_fn == nullptr)
    return osp::NodeStatus::kFailure;

  // Handle NAK: rewind to device's received_size
  if (ctx.data_nak_received) {
    ctx.data_nak_received = false;
    ++ctx.chunk_retry_count;
    ++ctx.total_retries;
    ++ctx.total_drops;
    if (ctx.chunk_retry_count > HostContext::kMaxChunkRetries) {
      OSP_LOG_ERROR("OTA_HOST", "Max retries (%u) at offset %u", HostContext::kMaxChunkRetries, ctx.last_received_size);
      return osp::NodeStatus::kFailure;
    }
    uint32_t rewind_to = ctx.last_received_size;
    OSP_LOG_INFO("OTA_HOST", "Rewind %u -> %u (retry %u/%u)", ctx.current_offset, rewind_to, ctx.chunk_retry_count,
                 HostContext::kMaxChunkRetries);
    if (ctx.current_offset > rewind_to) {
      ctx.bytes_sent -= (ctx.current_offset - rewind_to);
    }
    ctx.current_offset = rewind_to;
  }

  if (ctx.current_offset >= ctx.firmware_size) {
    OSP_LOG_INFO("OTA_HOST", "All chunks sent: %u bytes (%u retries)", ctx.bytes_sent, ctx.total_retries);
    return osp::NodeStatus::kSuccess;
  }

  uint32_t remaining = ctx.firmware_size - ctx.current_offset;
  uint32_t this_chunk = (remaining < ctx.chunk_size) ? remaining : ctx.chunk_size;

  uint8_t payload[kMaxPayloadLen];
  OtaDataHdr hdr;
  hdr.chunk_offset = ctx.current_offset;
  hdr.chunk_len = static_cast<uint16_t>(this_chunk);
  std::memcpy(payload, &hdr, sizeof(hdr));
  std::memcpy(&payload[sizeof(hdr)], &ctx.firmware_data[ctx.current_offset], this_chunk);

  uint8_t frame_buf[kMaxPayloadLen + kFrameOverhead + 4U];
  uint32_t n = BuildFrame(frame_buf, sizeof(frame_buf), static_cast<uint8_t>(CmdClass::kOta), ota_cmd::kData, payload,
                          static_cast<uint16_t>(sizeof(hdr) + this_chunk));
  if (n == 0)
    return osp::NodeStatus::kFailure;

  ctx.send_fn(frame_buf, n, ctx.send_ctx);
  ctx.current_offset += this_chunk;
  ctx.bytes_sent += this_chunk;
  // chunk_retry_count is reset in OnResponse when DATA ACK kSuccess is received

  if ((ctx.current_offset % (ctx.chunk_size * 8U)) == 0 || ctx.current_offset >= ctx.firmware_size) {
    OSP_LOG_INFO("OTA_HOST", "Progress: %u/%u (%.0f%%)", ctx.current_offset, ctx.firmware_size,
                 static_cast<double>(ctx.current_offset) * 100.0 / static_cast<double>(ctx.firmware_size));
  }
  return osp::NodeStatus::kRunning;
}

/// Send OTA_END, then wait for ACK.
inline osp::NodeStatus SendEnd(HostContext& ctx) {
  if (ctx.send_fn == nullptr)
    return osp::NodeStatus::kFailure;

  if (ctx.end_phase == ActionPhase::kDone) {
    return osp::NodeStatus::kSuccess;
  }

  if (ctx.end_phase == ActionPhase::kSend) {
    uint8_t frame_buf[32];
    uint32_t n =
        BuildFrame(frame_buf, sizeof(frame_buf), static_cast<uint8_t>(CmdClass::kOta), ota_cmd::kEnd, nullptr, 0);
    if (n == 0)
      return osp::NodeStatus::kFailure;

    // Reset state BEFORE send -- loopback may set ack_received synchronously
    ctx.ack_received = false;
    ctx.end_phase = ActionPhase::kWait;
    ctx.send_fn(frame_buf, n, ctx.send_ctx);
    OSP_LOG_INFO("OTA_HOST", "Sent END");
    return osp::NodeStatus::kRunning;
  }

  if (!ctx.ack_received)
    return osp::NodeStatus::kRunning;

  if (ctx.last_status == static_cast<uint8_t>(Status::kSuccess)) {
    ctx.end_phase = ActionPhase::kDone;
    OSP_LOG_INFO("OTA_HOST", "END ACK received");
    return osp::NodeStatus::kSuccess;
  }

  // END rejected (size mismatch) -- rewind chunks and retry
  if (ctx.last_received_size < ctx.firmware_size) {
    OSP_LOG_WARN("OTA_HOST", "END rejected: resend from offset %u", ctx.last_received_size);
    ctx.current_offset = ctx.last_received_size;
    ctx.end_phase = ActionPhase::kSend;
    ctx.ack_received = false;
    ctx.data_nak_received = false;
    return osp::NodeStatus::kRunning;
  }
  OSP_LOG_ERROR("OTA_HOST", "END ACK failed: status=%u", ctx.last_status);
  return osp::NodeStatus::kFailure;
}

/// Send OTA_VERIFY, then wait for verify response.
inline osp::NodeStatus SendVerify(HostContext& ctx) {
  if (ctx.send_fn == nullptr)
    return osp::NodeStatus::kFailure;

  if (ctx.verify_phase == ActionPhase::kDone) {
    return osp::NodeStatus::kSuccess;
  }

  if (ctx.verify_phase == ActionPhase::kSend) {
    uint8_t frame_buf[32];
    uint32_t n =
        BuildFrame(frame_buf, sizeof(frame_buf), static_cast<uint8_t>(CmdClass::kOta), ota_cmd::kVerify, nullptr, 0);
    if (n == 0)
      return osp::NodeStatus::kFailure;

    // Reset state BEFORE send -- loopback may set verify_received synchronously
    ctx.verify_received = false;
    ctx.verify_ok = false;
    ctx.verify_phase = ActionPhase::kWait;
    ctx.send_fn(frame_buf, n, ctx.send_ctx);
    OSP_LOG_INFO("OTA_HOST", "Sent VERIFY");
    return osp::NodeStatus::kRunning;
  }

  if (!ctx.verify_received)
    return osp::NodeStatus::kRunning;

  ctx.verify_phase = ActionPhase::kDone;
  if (ctx.verify_ok) {
    OSP_LOG_INFO("OTA_HOST", "Verify SUCCESS: CRC=0x%04X", ctx.verify_crc);
    return osp::NodeStatus::kSuccess;
  }
  OSP_LOG_ERROR("OTA_HOST", "Verify FAILED: expected=0x%04X got=0x%04X", ctx.expected_crc, ctx.verify_crc);
  return osp::NodeStatus::kFailure;
}

}  // namespace host_detail

// ============================================================================
// OtaHost Class
// ============================================================================

class OtaHost final {
 public:
  OtaHost(const uint8_t* fw_data, uint32_t fw_size, uint32_t start_addr, uint32_t chunk_size = 128) noexcept
      : tree_(ctx_, "ota_upgrade") {
    ctx_.firmware_data = fw_data;
    ctx_.firmware_size = fw_size;
    ctx_.start_addr = start_addr;
    ctx_.chunk_size = chunk_size;
  }

  OtaHost(const OtaHost&) = delete;
  OtaHost& operator=(const OtaHost&) = delete;

  void SetSendCallback(SendCallback fn, void* user_ctx) noexcept {
    ctx_.send_fn = fn;
    ctx_.send_ctx = user_ctx;
  }

  bool Start() noexcept {
    if (ctx_.firmware_data == nullptr || ctx_.firmware_size == 0 || ctx_.send_fn == nullptr) {
      return false;
    }

    ctx_.expected_crc = CalcCrc16(ctx_.firmware_data, ctx_.firmware_size);
    OSP_LOG_INFO("OTA_HOST", "Starting OTA: size=%u crc=0x%04X chunk=%u", ctx_.firmware_size, ctx_.expected_crc,
                 ctx_.chunk_size);

    auto root = tree_.AddSequence("ota_sequence", -1);
    tree_.AddAction("send_start", host_detail::SendStart, root);
    tree_.AddAction("send_chunks", host_detail::SendChunks, root);
    tree_.AddAction("send_end", host_detail::SendEnd, root);
    tree_.AddAction("send_verify", host_detail::SendVerify, root);
    tree_.SetRoot(root);
    return true;
  }

  osp::NodeStatus Tick() noexcept { return tree_.Tick(); }

  void OnResponse(const Frame& frame) noexcept {
    if (frame.cmd_class != static_cast<uint8_t>(CmdClass::kOta))
      return;
    if ((frame.cmd & ota_cmd::kAckFlag) == 0)
      return;

    uint8_t base_cmd = frame.cmd & ~ota_cmd::kAckFlag;

    if (base_cmd == ota_cmd::kVerify) {
      if (frame.data_len >= sizeof(OtaVerifyResp)) {
        OtaVerifyResp resp;
        std::memcpy(&resp, frame.data, sizeof(resp));
        ctx_.verify_crc = resp.calc_crc;
        ctx_.verify_ok = (resp.status == static_cast<uint8_t>(Status::kSuccess));
        ctx_.verify_received = true;
        OSP_LOG_DEBUG("OTA_HOST", "VERIFY resp: status=%u crc=0x%04X", resp.status, resp.calc_crc);
      }
    } else if (base_cmd == ota_cmd::kData) {
      // Data chunk ACK: check status for retransmission
      if (frame.data_len >= sizeof(OtaAckResp)) {
        OtaAckResp ack;
        std::memcpy(&ack, frame.data, sizeof(ack));
        if (ack.status != static_cast<uint8_t>(Status::kSuccess)) {
          // Device rejected the chunk -- rewind to received_size
          ctx_.last_received_size = ack.received_size;
          ctx_.data_nak_received = true;
          OSP_LOG_DEBUG("OTA_HOST", "Data NAK: status=%u received=%u", ack.status, ack.received_size);
        } else {
          // Chunk confirmed -- reset retry count for this offset
          ctx_.chunk_retry_count = 0;
        }
      }
    } else {
      if (frame.data_len >= sizeof(OtaAckResp)) {
        OtaAckResp ack;
        std::memcpy(&ack, frame.data, sizeof(ack));
        ctx_.last_status = ack.status;
        ctx_.last_received_size = ack.received_size;
        ctx_.ack_received = true;
      }
    }
  }

  float GetProgress() const noexcept {
    if (ctx_.firmware_size == 0)
      return 0.0f;
    return static_cast<float>(ctx_.bytes_sent) / static_cast<float>(ctx_.firmware_size);
  }
  uint32_t GetBytesSent() const noexcept { return ctx_.bytes_sent; }
  bool IsComplete() const noexcept { return tree_.LastStatus() == osp::NodeStatus::kSuccess; }
  bool IsFailed() const noexcept { return tree_.LastStatus() == osp::NodeStatus::kFailure; }
  osp::NodeStatus GetStatus() const noexcept { return tree_.LastStatus(); }
  uint32_t GetTotalRetries() const noexcept { return ctx_.total_retries; }
  uint32_t GetTotalDrops() const noexcept { return ctx_.total_drops; }

 private:
  HostContext ctx_;
  osp::BehaviorTree<HostContext, 16> tree_;
};

}  // namespace ota

#endif  // SERIAL_OTA_HOST_HPP_
