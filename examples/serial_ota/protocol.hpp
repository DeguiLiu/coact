/**
 * @file protocol.hpp
 * @brief UART protocol definitions (compatible with uart_statemachine_ringbuffer_linux).
 *
 * Frame format:
 *   0xAA | LEN_LO | LEN_HI | CMD_CLASS | CMD | DATA[LEN-2] | CRC_LO | CRC_HI | 0x55
 *
 * All multi-byte fields are little-endian. Structs are packed for wire format.
 */

#ifndef SERIAL_OTA_PROTOCOL_HPP_
#define SERIAL_OTA_PROTOCOL_HPP_

#include <cstdint>
#include <cstring>

#include <type_traits>

namespace ota {

// ============================================================================
// Frame Constants
// ============================================================================

static constexpr uint8_t kFrameHeader = 0xAAU;
static constexpr uint8_t kFrameTail = 0x55U;
static constexpr uint16_t kMaxPayloadLen = 256U;
static constexpr uint16_t kFrameOverhead = 6U;  // header(1) + len(2) + crc(2) + tail(1)

// ============================================================================
// Command Classes (from uart_protocol.h)
// ============================================================================

enum class CmdClass : uint8_t {
  kSys = 0x01U,
  kSpi = 0x02U,
  kDiag = 0x03U,
  kOta = 0x04U,
  kConfig = 0x10U,
};

// ============================================================================
// Command IDs
// ============================================================================

namespace sys {
static constexpr uint8_t kGetInfo = 0x81U;
static constexpr uint8_t kRstNormal = 0x41U;
static constexpr uint8_t kRstBoot = 0x47U;
static constexpr uint8_t kSetBaud = 0x44U;
}  // namespace sys

namespace spi {
static constexpr uint8_t kReadId = 0x81U;
static constexpr uint8_t kRead = 0x82U;
static constexpr uint8_t kWrite = 0xC1U;
static constexpr uint8_t kErase = 0x44U;
static constexpr uint8_t kGetCrc = 0x84U;
}  // namespace spi

namespace ota_cmd {
static constexpr uint8_t kStart = 0x01U;
static constexpr uint8_t kData = 0x02U;
static constexpr uint8_t kEnd = 0x03U;
static constexpr uint8_t kVerify = 0x04U;
static constexpr uint8_t kAckFlag = 0x80U;  // OR'd into cmd for responses
}  // namespace ota_cmd

// ============================================================================
// Status Codes
// ============================================================================

enum class Status : uint8_t {
  kSuccess = 0x00U,
  kUnknown = 0x01U,
  kParamErr = 0x02U,
  kCrcErr = 0x03U,
  kHwErr = 0x04U,
  kBusy = 0x05U,
  kTimeout = 0x06U,
};

// ============================================================================
// CRC-CCITT (constexpr table generation)
// ============================================================================

namespace detail {

struct Crc16Table {
  uint16_t data[256];

  constexpr Crc16Table() : data{} {
    for (uint32_t i = 0; i < 256; ++i) {
      uint16_t crc = static_cast<uint16_t>(i << 8);
      for (uint32_t j = 0; j < 8; ++j) {
        crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1) ^ 0x1021U) : static_cast<uint16_t>(crc << 1);
      }
      data[i] = crc;
    }
  }
};

static constexpr Crc16Table kCrc16Table{};

}  // namespace detail

/// Calculate CRC-CCITT over a byte range.
inline uint16_t CalcCrc16(const uint8_t* data, uint32_t len) noexcept {
  uint16_t crc = 0x0000U;
  for (uint32_t i = 0; i < len; ++i) {
    crc = static_cast<uint16_t>((crc << 8) ^ detail::kCrc16Table.data[static_cast<uint8_t>((crc >> 8) ^ data[i])]);
  }
  return crc;
}

// ============================================================================
// Wire Structures (packed, byte-aligned)
// ============================================================================

#pragma pack(push, 1)

/// Parsed frame (after protocol decoding).
struct Frame {
  uint8_t cmd_class;
  uint8_t cmd;
  uint16_t data_len;
  uint8_t data[kMaxPayloadLen];
  uint16_t crc;
};

/// OTA START request payload.
struct OtaStartReq {
  uint32_t total_size;
  uint32_t start_addr;
  uint16_t expected_crc;
};

/// OTA DATA request payload header.
struct OtaDataHdr {
  uint32_t chunk_offset;
  uint16_t chunk_len;
};

/// OTA ACK response.
struct OtaAckResp {
  uint8_t cmd_class;
  uint8_t cmd;
  uint8_t status;
  uint32_t received_size;
};

/// OTA VERIFY response.
struct OtaVerifyResp {
  uint8_t cmd_class;
  uint8_t cmd;
  uint8_t status;
  uint16_t calc_crc;
};

#pragma pack(pop)

static_assert(sizeof(OtaStartReq) == 10, "OtaStartReq must be 10 bytes");
static_assert(sizeof(OtaDataHdr) == 6, "OtaDataHdr must be 6 bytes");
static_assert(sizeof(OtaAckResp) == 7, "OtaAckResp must be 7 bytes");
static_assert(sizeof(OtaVerifyResp) == 5, "OtaVerifyResp must be 5 bytes");

// ============================================================================
// Frame Builder (zero-copy, stack-allocated)
// ============================================================================

/// Build a protocol frame into a caller-provided buffer.
/// Returns total frame length, or 0 on error.
inline uint32_t BuildFrame(uint8_t* buf, uint32_t buf_size, uint8_t cmd_class, uint8_t cmd, const void* data,
                           uint16_t data_len) noexcept {
  uint16_t payload_len = static_cast<uint16_t>(2U + data_len);
  uint32_t total = static_cast<uint32_t>(payload_len) + kFrameOverhead;
  if (buf == nullptr || total > buf_size)
    return 0;

  uint32_t idx = 0;
  buf[idx++] = kFrameHeader;
  buf[idx++] = static_cast<uint8_t>(payload_len & 0xFFU);
  buf[idx++] = static_cast<uint8_t>((payload_len >> 8) & 0xFFU);
  buf[idx++] = cmd_class;
  buf[idx++] = cmd;

  if (data != nullptr && data_len > 0) {
    std::memcpy(&buf[idx], data, data_len);
    idx += data_len;
  }

  uint16_t crc = CalcCrc16(&buf[3], payload_len);
  buf[idx++] = static_cast<uint8_t>(crc & 0xFFU);
  buf[idx++] = static_cast<uint8_t>((crc >> 8) & 0xFFU);
  buf[idx++] = kFrameTail;

  return idx;
}

}  // namespace ota

#endif  // SERIAL_OTA_PROTOCOL_HPP_
