/**
 * @file device.hpp
 * @brief Device-side OTA module: flash simulation, HSM state machine,
 *        command dispatch.
 *
 * Modern C++17 rewrite of cmd_handler.c from the reference project.
 * Uses osp::StateMachine for the OTA state machine.
 */

#ifndef SERIAL_OTA_DEVICE_HPP_
#define SERIAL_OTA_DEVICE_HPP_

#include "protocol.hpp"

#include "osp/hsm.hpp"
#include "osp/log.hpp"

#include <cstdint>
#include <cstring>

namespace ota {

// ============================================================================
// Flash Simulation
// ============================================================================

template <uint32_t FlashSize = 65536U, uint32_t SectorSize = 4096U>
class FlashSim {
 public:
  FlashSim() noexcept { std::memset(flash_, 0xFFU, FlashSize); }

  bool Read(uint32_t addr, uint8_t* buf, uint32_t len) const noexcept {
    if (buf == nullptr || addr + len > FlashSize)
      return false;
    std::memcpy(buf, &flash_[addr], len);
    return true;
  }

  bool Write(uint32_t addr, const uint8_t* data, uint32_t len) noexcept {
    if (data == nullptr || addr + len > FlashSize)
      return false;
    std::memcpy(&flash_[addr], data, len);
    return true;
  }

  bool EraseSector(uint32_t addr) noexcept {
    uint32_t aligned = addr & ~(SectorSize - 1U);
    if (aligned + SectorSize > FlashSize)
      return false;
    std::memset(&flash_[aligned], 0xFFU, SectorSize);
    return true;
  }

  uint16_t CalcCrc(uint32_t addr, uint32_t len) const noexcept {
    if (addr + len > FlashSize)
      return 0;
    return CalcCrc16(&flash_[addr], len);
  }

  const uint8_t* Data() const noexcept { return flash_; }
  static constexpr uint32_t Size() noexcept { return FlashSize; }
  static constexpr uint32_t Sector() noexcept { return SectorSize; }

 private:
  uint8_t flash_[FlashSize];
};

// ============================================================================
// Device Information
// ============================================================================

struct DeviceInfo {
  const char* name = "OTA-Device-V1";
  const char* fw_version = "1.0.0";
  const char* boot_version = "0.9.5";
  uint16_t vendor_id = 0x1234U;
  uint16_t product_id = 0x5678U;
  const char* serial_number = "SN20250214001";
};

// ============================================================================
// OTA State Machine
// ============================================================================

/// OTA-specific event IDs (distinct from parser events).
enum OtaEvt : uint32_t {
  kEvtOtaStart = 10U,
  kEvtOtaData = 11U,
  kEvtOtaEnd = 12U,
  kEvtOtaVerify = 13U,
  kEvtOtaReset = 20U,
};

/// Per-instance state indices for the OTA state machine.
struct OtaStateIdx {
  int32_t idle = -1;
  int32_t erasing = -1;
  int32_t receiving = -1;
  int32_t verifying = -1;
  int32_t complete = -1;
  int32_t error = -1;
};

/// Context shared between OTA state handlers and DeviceHandler.
template <uint32_t FlashSize, uint32_t SectorSize>
struct DeviceContext {
  FlashSim<FlashSize, SectorSize>* flash = nullptr;
  osp::StateMachine<DeviceContext, 8>* sm = nullptr;
  OtaStateIdx si;

  // OTA session
  uint32_t total_size = 0;
  uint32_t start_addr = 0;
  uint32_t received_size = 0;
  uint16_t expected_crc = 0;
  uint16_t calc_crc = 0;
  bool erase_ok = true;
};

// --- State handlers (inline free functions) ---------------------------------

namespace device_detail {

template <uint32_t FS, uint32_t SS>
inline osp::TransitionResult OnIdle(DeviceContext<FS, SS>& ctx, const osp::Event& evt) noexcept {
  if (evt.id == kEvtOtaStart && evt.data != nullptr) {
    const auto* req = static_cast<const OtaStartReq*>(evt.data);
    ctx.total_size = req->total_size;
    ctx.start_addr = req->start_addr;
    ctx.expected_crc = req->expected_crc;
    ctx.received_size = 0;
    ctx.calc_crc = 0;
    ctx.erase_ok = true;
    OSP_LOG_INFO("OTA_DEV", "OTA Start: size=%u addr=0x%X crc=0x%04X", ctx.total_size, ctx.start_addr,
                 ctx.expected_crc);
    return ctx.sm->RequestTransition(ctx.si.erasing);
  }
  return osp::TransitionResult::kUnhandled;
}

template <uint32_t FS, uint32_t SS>
inline void OnEnterErasing(DeviceContext<FS, SS>& ctx) noexcept {
  OSP_LOG_INFO("OTA_DEV", "Erasing flash sectors...");
  uint32_t end_addr = ctx.start_addr + ctx.total_size;
  for (uint32_t a = ctx.start_addr; a < end_addr; a += SS) {
    if (!ctx.flash->EraseSector(a)) {
      OSP_LOG_ERROR("OTA_DEV", "Erase failed at 0x%X", a);
      ctx.erase_ok = false;
      return;
    }
  }
  OSP_LOG_INFO("OTA_DEV", "Erase complete");
}

template <uint32_t FS, uint32_t SS>
inline osp::TransitionResult OnErasing(DeviceContext<FS, SS>& ctx, const osp::Event&) noexcept {
  // Entry action did the work; transition based on result.
  if (!ctx.erase_ok) {
    return ctx.sm->RequestTransition(ctx.si.error);
  }
  return ctx.sm->RequestTransition(ctx.si.receiving);
}

template <uint32_t FS, uint32_t SS>
inline osp::TransitionResult OnReceiving(DeviceContext<FS, SS>& ctx, const osp::Event& evt) noexcept {
  if (evt.id == kEvtOtaData && evt.data != nullptr) {
    const auto* hdr = static_cast<const OtaDataHdr*>(evt.data);
    const uint8_t* chunk = static_cast<const uint8_t*>(evt.data) + sizeof(OtaDataHdr);

    if (hdr->chunk_offset != ctx.received_size) {
      OSP_LOG_ERROR("OTA_DEV", "Offset mismatch: expect=%u got=%u", ctx.received_size, hdr->chunk_offset);
      return ctx.sm->RequestTransition(ctx.si.error);
    }

    uint32_t wa = ctx.start_addr + hdr->chunk_offset;
    if (!ctx.flash->Write(wa, chunk, hdr->chunk_len)) {
      OSP_LOG_ERROR("OTA_DEV", "Write failed at 0x%X", wa);
      return ctx.sm->RequestTransition(ctx.si.error);
    }

    ctx.received_size += hdr->chunk_len;
    OSP_LOG_DEBUG("OTA_DEV", "Chunk: off=%u len=%u total=%u/%u", hdr->chunk_offset, hdr->chunk_len, ctx.received_size,
                  ctx.total_size);
    return osp::TransitionResult::kHandled;
  }

  if (evt.id == kEvtOtaEnd) {
    if (ctx.received_size != ctx.total_size) {
      OSP_LOG_ERROR("OTA_DEV", "Size mismatch: expect=%u got=%u", ctx.total_size, ctx.received_size);
      return ctx.sm->RequestTransition(ctx.si.error);
    }
    OSP_LOG_INFO("OTA_DEV", "All data received");
    return ctx.sm->RequestTransition(ctx.si.verifying);
  }
  return osp::TransitionResult::kUnhandled;
}

template <uint32_t FS, uint32_t SS>
inline void OnEnterVerifying(DeviceContext<FS, SS>& ctx) noexcept {
  ctx.calc_crc = ctx.flash->CalcCrc(ctx.start_addr, ctx.total_size);
  OSP_LOG_INFO("OTA_DEV", "CRC: expected=0x%04X calculated=0x%04X", ctx.expected_crc, ctx.calc_crc);
}

template <uint32_t FS, uint32_t SS>
inline osp::TransitionResult OnVerifying(DeviceContext<FS, SS>& ctx, const osp::Event& evt) noexcept {
  if (evt.id == kEvtOtaVerify) {
    if (ctx.calc_crc == ctx.expected_crc) {
      OSP_LOG_INFO("OTA_DEV", "CRC match -- OTA complete");
      return ctx.sm->RequestTransition(ctx.si.complete);
    }
    OSP_LOG_ERROR("OTA_DEV", "CRC mismatch");
    return ctx.sm->RequestTransition(ctx.si.error);
  }
  return osp::TransitionResult::kUnhandled;
}

template <uint32_t FS, uint32_t SS>
inline void OnEnterComplete(DeviceContext<FS, SS>&) noexcept {
  OSP_LOG_INFO("OTA_DEV", "OTA completed successfully");
}

template <uint32_t FS, uint32_t SS>
inline osp::TransitionResult OnComplete(DeviceContext<FS, SS>& ctx, const osp::Event& evt) noexcept {
  if (evt.id == kEvtOtaReset) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kHandled;
}

template <uint32_t FS, uint32_t SS>
inline void OnEnterError(DeviceContext<FS, SS>&) noexcept {
  OSP_LOG_ERROR("OTA_DEV", "Entered error state");
}

template <uint32_t FS, uint32_t SS>
inline osp::TransitionResult OnError(DeviceContext<FS, SS>& ctx, const osp::Event& evt) noexcept {
  if (evt.id == kEvtOtaReset) {
    return ctx.sm->RequestTransition(ctx.si.idle);
  }
  return osp::TransitionResult::kHandled;
}

}  // namespace device_detail

// ============================================================================
// DeviceHandler
// ============================================================================

template <uint32_t FlashSize = 65536U, uint32_t SectorSize = 4096U>
class DeviceHandler final {
 public:
  using Flash = FlashSim<FlashSize, SectorSize>;
  using Context = DeviceContext<FlashSize, SectorSize>;
  using RespCb = void (*)(const uint8_t* data, uint32_t len, void* ctx);

  DeviceHandler() noexcept : sm_(ctx_) {
    ctx_.flash = &flash_;
    ctx_.sm = &sm_;
    BuildStateMachine();
  }

  DeviceHandler(const DeviceHandler&) = delete;
  DeviceHandler& operator=(const DeviceHandler&) = delete;

  void SetResponseCallback(RespCb cb, void* user_ctx) noexcept {
    resp_cb_ = cb;
    resp_ctx_ = user_ctx;
  }

  /// Dispatch a parsed frame to the appropriate command handler.
  void ProcessFrame(const Frame& frame) noexcept {
    switch (frame.cmd_class) {
      case static_cast<uint8_t>(CmdClass::kSys):
        HandleSys(frame);
        break;
      case static_cast<uint8_t>(CmdClass::kSpi):
        HandleSpi(frame);
        break;
      case static_cast<uint8_t>(CmdClass::kOta):
        HandleOta(frame);
        break;
      default:
        OSP_LOG_WARN("OTA_DEV", "Unknown class: 0x%02X", frame.cmd_class);
        SendStatus(frame.cmd_class, frame.cmd, Status::kUnknown);
        break;
    }
  }

  const char* GetOtaStateName() const noexcept { return sm_.CurrentStateName(); }
  const Flash& GetFlash() const noexcept { return flash_; }
  const Context& GetContext() const noexcept { return ctx_; }

 private:
  // --- State machine setup --------------------------------------------------

  void BuildStateMachine() noexcept {
    using SC = osp::StateConfig<Context>;
    namespace D = device_detail;

    ctx_.si.idle = sm_.AddState(SC{"Idle", -1, D::OnIdle<FlashSize, SectorSize>, nullptr, nullptr, nullptr});
    ctx_.si.erasing = sm_.AddState(SC{"Erasing", -1, D::OnErasing<FlashSize, SectorSize>,
                                      D::OnEnterErasing<FlashSize, SectorSize>, nullptr, nullptr});
    ctx_.si.receiving =
        sm_.AddState(SC{"Receiving", -1, D::OnReceiving<FlashSize, SectorSize>, nullptr, nullptr, nullptr});
    ctx_.si.verifying = sm_.AddState(SC{"Verifying", -1, D::OnVerifying<FlashSize, SectorSize>,
                                        D::OnEnterVerifying<FlashSize, SectorSize>, nullptr, nullptr});
    ctx_.si.complete = sm_.AddState(SC{"Complete", -1, D::OnComplete<FlashSize, SectorSize>,
                                       D::OnEnterComplete<FlashSize, SectorSize>, nullptr, nullptr});
    ctx_.si.error = sm_.AddState(
        SC{"Error", -1, D::OnError<FlashSize, SectorSize>, D::OnEnterError<FlashSize, SectorSize>, nullptr, nullptr});

    sm_.SetInitialState(ctx_.si.idle);
    sm_.Start();
  }

  // --- SYS commands ---------------------------------------------------------

  void HandleSys(const Frame& frame) noexcept {
    if (frame.cmd == sys::kGetInfo) {
      uint8_t buf[128];
      uint32_t off = 0;
      auto put_str = [&](const char* s) {
        auto len = static_cast<uint8_t>(std::strlen(s));
        buf[off++] = len;
        std::memcpy(&buf[off], s, len);
        off += len;
      };
      put_str(info_.name);
      put_str(info_.fw_version);
      put_str(info_.boot_version);
      std::memcpy(&buf[off], &info_.vendor_id, 2);
      off += 2;
      std::memcpy(&buf[off], &info_.product_id, 2);
      off += 2;
      put_str(info_.serial_number);
      SendResp(frame.cmd_class, frame.cmd, buf, static_cast<uint16_t>(off));
      OSP_LOG_INFO("OTA_DEV", "SYS GetInfo sent");
    } else {
      SendStatus(frame.cmd_class, frame.cmd, Status::kUnknown);
    }
  }

  // --- SPI commands ---------------------------------------------------------

  void HandleSpi(const Frame& frame) noexcept {
    switch (frame.cmd) {
      case spi::kReadId: {
        uint8_t id[3] = {0xEFU, 0x40U, 0x18U};
        SendResp(frame.cmd_class, frame.cmd, id, 3);
        break;
      }
      case spi::kRead: {
        if (frame.data_len < 6) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kParamErr);
          break;
        }
        uint32_t addr = 0;
        uint16_t len = 0;
        std::memcpy(&addr, frame.data, 4);
        std::memcpy(&len, &frame.data[4], 2);
        uint8_t rb[256];
        if (len > sizeof(rb) || !flash_.Read(addr, rb, len)) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kParamErr);
          break;
        }
        SendResp(frame.cmd_class, frame.cmd, rb, len);
        break;
      }
      case spi::kWrite: {
        if (frame.data_len < 7) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kParamErr);
          break;
        }
        uint32_t addr = 0;
        uint16_t len = 0;
        std::memcpy(&addr, frame.data, 4);
        std::memcpy(&len, &frame.data[4], 2);
        if (!flash_.Write(addr, &frame.data[6], len)) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kHwErr);
          break;
        }
        SendStatus(frame.cmd_class, frame.cmd, Status::kSuccess);
        break;
      }
      case spi::kErase: {
        if (frame.data_len < 4) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kParamErr);
          break;
        }
        uint32_t addr = 0;
        std::memcpy(&addr, frame.data, 4);
        if (!flash_.EraseSector(addr)) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kHwErr);
          break;
        }
        SendStatus(frame.cmd_class, frame.cmd, Status::kSuccess);
        break;
      }
      case spi::kGetCrc: {
        if (frame.data_len < 8) {
          SendStatus(frame.cmd_class, frame.cmd, Status::kParamErr);
          break;
        }
        uint32_t addr = 0, len = 0;
        std::memcpy(&addr, frame.data, 4);
        std::memcpy(&len, &frame.data[4], 4);
        uint16_t crc = flash_.CalcCrc(addr, len);
        SendResp(frame.cmd_class, frame.cmd, reinterpret_cast<const uint8_t*>(&crc), 2);
        break;
      }
      default:
        SendStatus(frame.cmd_class, frame.cmd, Status::kUnknown);
        break;
    }
  }

  // --- OTA commands ---------------------------------------------------------

  void HandleOta(const Frame& frame) noexcept {
    switch (frame.cmd) {
      case ota_cmd::kStart: {
        if (frame.data_len < sizeof(OtaStartReq)) {
          SendOtaAck(frame.cmd, Status::kParamErr);
          break;
        }
        sm_.Dispatch(osp::Event{kEvtOtaStart, frame.data});
        // After dispatch, Erasing entry runs synchronously.
        // Then OnErasing handler transitions to Receiving or Error.
        // We need a second dispatch to trigger that handler.
        sm_.Dispatch(osp::Event{0, nullptr});  // trigger pending transition
        SendOtaAck(frame.cmd, Status::kSuccess);
        break;
      }
      case ota_cmd::kData: {
        if (frame.data_len < sizeof(OtaDataHdr)) {
          SendOtaAck(frame.cmd, Status::kParamErr);
          break;
        }
        // Check offset before dispatching to HSM
        OtaDataHdr hdr;
        std::memcpy(&hdr, frame.data, sizeof(hdr));
        if (hdr.chunk_offset != ctx_.received_size) {
          OSP_LOG_WARN("OTA_DEV", "Offset mismatch: expect=%u got=%u", ctx_.received_size, hdr.chunk_offset);
          SendOtaAck(frame.cmd, Status::kParamErr);
          break;
        }
        sm_.Dispatch(osp::Event{kEvtOtaData, frame.data});
        SendOtaAck(frame.cmd, Status::kSuccess);
        break;
      }
      case ota_cmd::kEnd: {
        if (ctx_.received_size != ctx_.total_size) {
          // Size mismatch -- last chunk may have been lost
          OSP_LOG_WARN("OTA_DEV", "END rejected: received=%u expected=%u", ctx_.received_size, ctx_.total_size);
          SendOtaAck(frame.cmd, Status::kParamErr);
          break;
        }
        sm_.Dispatch(osp::Event{kEvtOtaEnd, nullptr});
        SendOtaAck(frame.cmd, Status::kSuccess);
        break;
      }
      case ota_cmd::kVerify: {
        sm_.Dispatch(osp::Event{kEvtOtaVerify, nullptr});
        OtaVerifyResp resp;
        resp.cmd_class = static_cast<uint8_t>(CmdClass::kOta);
        resp.cmd = frame.cmd | ota_cmd::kAckFlag;
        resp.status = (ctx_.calc_crc == ctx_.expected_crc) ? static_cast<uint8_t>(Status::kSuccess)
                                                           : static_cast<uint8_t>(Status::kCrcErr);
        resp.calc_crc = ctx_.calc_crc;
        SendResp(static_cast<uint8_t>(CmdClass::kOta), frame.cmd | ota_cmd::kAckFlag,
                 reinterpret_cast<const uint8_t*>(&resp), sizeof(resp));
        break;
      }
      default:
        SendStatus(frame.cmd_class, frame.cmd, Status::kUnknown);
        break;
    }
  }

  // --- Response helpers -----------------------------------------------------

  void SendResp(uint8_t cls, uint8_t cmd, const uint8_t* data, uint16_t len) noexcept {
    if (resp_cb_ == nullptr)
      return;
    uint8_t buf[512];
    uint32_t n = BuildFrame(buf, sizeof(buf), cls, cmd, data, len);
    if (n > 0)
      resp_cb_(buf, n, resp_ctx_);
  }

  void SendStatus(uint8_t cls, uint8_t cmd, Status s) noexcept {
    uint8_t st = static_cast<uint8_t>(s);
    SendResp(cls, cmd, &st, 1);
  }

  void SendOtaAck(uint8_t cmd, Status s) noexcept {
    OtaAckResp ack;
    ack.cmd_class = static_cast<uint8_t>(CmdClass::kOta);
    ack.cmd = cmd | ota_cmd::kAckFlag;
    ack.status = static_cast<uint8_t>(s);
    ack.received_size = ctx_.received_size;
    SendResp(static_cast<uint8_t>(CmdClass::kOta), cmd | ota_cmd::kAckFlag, reinterpret_cast<const uint8_t*>(&ack),
             sizeof(ack));
  }

  // --- Members --------------------------------------------------------------

  Flash flash_;
  DeviceInfo info_;
  Context ctx_;
  osp::StateMachine<Context, 8> sm_;
  RespCb resp_cb_ = nullptr;
  void* resp_ctx_ = nullptr;
};

}  // namespace ota

#endif  // SERIAL_OTA_DEVICE_HPP_
