/**
 * @file parser.hpp
 * @brief HSM-based protocol frame parser using osp::StateMachine.
 *
 * Modern C++17 rewrite of hsm_parser.c from the reference project.
 * Uses osp::StateMachine<ParserContext, 10> for byte-by-byte frame parsing.
 *
 * Protocol frame format:
 *   0xAA | LEN_LO | LEN_HI | CMD_CLASS | CMD | DATA[LEN-2] | CRC_LO | CRC_HI | 0x55
 *
 * States: Idle, LenLo, LenHi, CmdClass, Cmd, Data, CrcLo, CrcHi, Tail
 * Events: kEvtByte, kEvtReset, kEvtTimeout
 */

#ifndef SERIAL_OTA_PARSER_HPP_
#define SERIAL_OTA_PARSER_HPP_

#include "protocol.hpp"

#include "osp/hsm.hpp"
#include "osp/log.hpp"

#include <cstdint>
#include <cstring>

namespace ota {

// ============================================================================
// Event IDs
// ============================================================================

static constexpr uint32_t kEvtByte = 1U;
static constexpr uint32_t kEvtReset = 2U;
static constexpr uint32_t kEvtTimeout = 3U;

// ============================================================================
// Parser Statistics
// ============================================================================

struct ParserStats {
  uint32_t bytes_received = 0;
  uint32_t frames_received = 0;
  uint32_t sync_errors = 0;
  uint32_t crc_errors = 0;
  uint32_t tail_errors = 0;
  uint32_t length_errors = 0;
};

// ============================================================================
// Frame Callback
// ============================================================================

using FrameCallback = void (*)(const Frame& frame, void* user_data);

// ============================================================================
// Parser Context
// ============================================================================

/// Per-instance state indices (avoids mutable statics).
struct ParserStateIdx {
  int32_t idle = -1;
  int32_t len_lo = -1;
  int32_t len_hi = -1;
  int32_t cmd_class = -1;
  int32_t cmd = -1;
  int32_t data = -1;
  int32_t crc_lo = -1;
  int32_t crc_hi = -1;
  int32_t tail = -1;
};

struct ParserContext {
  uint8_t current_byte = 0;
  Frame frame = {};
  uint16_t expected_len = 0;
  uint16_t payload_index = 0;
  uint8_t payload_buf[kMaxPayloadLen + 2U] = {};
  ParserStats stats = {};
  FrameCallback callback = nullptr;
  void* user_data = nullptr;

  osp::StateMachine<ParserContext, 10>* sm = nullptr;
  ParserStateIdx si;
};

// ============================================================================
// State Handlers (inline free functions)
// ============================================================================

namespace parser_detail {

inline osp::TransitionResult HandleIdle(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    if (ctx.current_byte == kFrameHeader) {
      std::memset(&ctx.frame, 0, sizeof(ctx.frame));
      ctx.expected_len = 0;
      ctx.payload_index = 0;
      return ctx.sm->RequestTransition(ctx.si.len_lo);
    }
    ++ctx.stats.sync_errors;
    return osp::TransitionResult::kHandled;
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return osp::TransitionResult::kHandled;
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleLenLo(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    ctx.expected_len = ctx.current_byte;
    return ctx.sm->RequestTransition(ctx.si.len_hi);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleLenHi(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    ctx.expected_len = static_cast<uint16_t>(ctx.expected_len | static_cast<uint16_t>(ctx.current_byte << 8U));

    if (ctx.expected_len < 2U || (ctx.expected_len - 2U) > kMaxPayloadLen) {
      ++ctx.stats.length_errors;
      OSP_LOG_WARN("OTA_PARSER", "Invalid length: %u", ctx.expected_len);
      return ctx.sm->RequestTransition(ctx.si.idle);
    }
    return ctx.sm->RequestTransition(ctx.si.cmd_class);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleCmdClass(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    ctx.frame.cmd_class = ctx.current_byte;
    ctx.payload_buf[ctx.payload_index++] = ctx.current_byte;
    return ctx.sm->RequestTransition(ctx.si.cmd);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleCmd(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    ctx.frame.cmd = ctx.current_byte;
    ctx.payload_buf[ctx.payload_index++] = ctx.current_byte;
    ctx.frame.data_len = static_cast<uint16_t>(ctx.expected_len - 2U);

    if (ctx.frame.data_len > 0U) {
      return ctx.sm->RequestTransition(ctx.si.data);
    }
    return ctx.sm->RequestTransition(ctx.si.crc_lo);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleData(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    uint16_t data_offset = static_cast<uint16_t>(ctx.payload_index - 2U);
    ctx.frame.data[data_offset] = ctx.current_byte;
    ctx.payload_buf[ctx.payload_index++] = ctx.current_byte;

    if (ctx.payload_index >= ctx.expected_len) {
      return ctx.sm->RequestTransition(ctx.si.crc_lo);
    }
    return osp::TransitionResult::kHandled;
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleCrcLo(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    ctx.frame.crc = ctx.current_byte;
    return ctx.sm->RequestTransition(ctx.si.crc_hi);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleCrcHi(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    ctx.frame.crc = static_cast<uint16_t>(ctx.frame.crc | static_cast<uint16_t>(ctx.current_byte << 8U));
    return ctx.sm->RequestTransition(ctx.si.tail);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

inline osp::TransitionResult HandleTail(ParserContext& ctx, const osp::Event& event) noexcept {
  if (event.id == kEvtByte) {
    if (ctx.current_byte != kFrameTail) {
      ++ctx.stats.tail_errors;
      OSP_LOG_WARN("OTA_PARSER", "Bad tail: 0x%02X", ctx.current_byte);
      return ctx.sm->RequestTransition(ctx.si.idle);
    }

    uint16_t calc_crc = CalcCrc16(ctx.payload_buf, ctx.expected_len);
    if (calc_crc != ctx.frame.crc) {
      ++ctx.stats.crc_errors;
      OSP_LOG_WARN("OTA_PARSER", "CRC mismatch: calc=0x%04X recv=0x%04X", calc_crc, ctx.frame.crc);
      return ctx.sm->RequestTransition(ctx.si.idle);
    }

    ++ctx.stats.frames_received;
    OSP_LOG_DEBUG("OTA_PARSER", "Frame OK: class=0x%02X cmd=0x%02X len=%u", ctx.frame.cmd_class, ctx.frame.cmd,
                  ctx.frame.data_len);

    if (ctx.callback != nullptr) {
      ctx.callback(ctx.frame, ctx.user_data);
    }
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  if (event.id == kEvtReset || event.id == kEvtTimeout) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kUnhandled;
}

}  // namespace parser_detail

// ============================================================================
// FrameParser Class
// ============================================================================

class FrameParser final {
 public:
  FrameParser() noexcept : ctx_{}, sm_(ctx_) {
    ctx_.sm = &sm_;

    using SC = osp::StateConfig<ParserContext>;
    ctx_.si.idle = sm_.AddState(SC{"Idle", -1, parser_detail::HandleIdle, nullptr, nullptr, nullptr});
    ctx_.si.len_lo = sm_.AddState(SC{"LenLo", -1, parser_detail::HandleLenLo, nullptr, nullptr, nullptr});
    ctx_.si.len_hi = sm_.AddState(SC{"LenHi", -1, parser_detail::HandleLenHi, nullptr, nullptr, nullptr});
    ctx_.si.cmd_class = sm_.AddState(SC{"CmdClass", -1, parser_detail::HandleCmdClass, nullptr, nullptr, nullptr});
    ctx_.si.cmd = sm_.AddState(SC{"Cmd", -1, parser_detail::HandleCmd, nullptr, nullptr, nullptr});
    ctx_.si.data = sm_.AddState(SC{"Data", -1, parser_detail::HandleData, nullptr, nullptr, nullptr});
    ctx_.si.crc_lo = sm_.AddState(SC{"CrcLo", -1, parser_detail::HandleCrcLo, nullptr, nullptr, nullptr});
    ctx_.si.crc_hi = sm_.AddState(SC{"CrcHi", -1, parser_detail::HandleCrcHi, nullptr, nullptr, nullptr});
    ctx_.si.tail = sm_.AddState(SC{"Tail", -1, parser_detail::HandleTail, nullptr, nullptr, nullptr});

    sm_.SetInitialState(ctx_.si.idle);
  }

  FrameParser(const FrameParser&) = delete;
  FrameParser& operator=(const FrameParser&) = delete;

  void SetCallback(FrameCallback callback, void* user_data = nullptr) noexcept {
    ctx_.callback = callback;
    ctx_.user_data = user_data;
  }

  void Start() noexcept { sm_.Start(); }

  void PutByte(uint8_t byte) noexcept {
    ctx_.current_byte = byte;
    ++ctx_.stats.bytes_received;
    sm_.Dispatch(osp::Event{kEvtByte, nullptr});
  }

  void PutData(const uint8_t* data, uint32_t len) noexcept {
    for (uint32_t i = 0; i < len; ++i) {
      PutByte(data[i]);
    }
  }

  void Reset() noexcept { sm_.Dispatch(osp::Event{kEvtReset, nullptr}); }

  const ParserStats& GetStats() const noexcept { return ctx_.stats; }
  const char* CurrentStateName() const noexcept { return sm_.CurrentStateName(); }

 private:
  ParserContext ctx_;
  osp::StateMachine<ParserContext, 10> sm_;
};

}  // namespace ota

#endif  // SERIAL_OTA_PARSER_HPP_
