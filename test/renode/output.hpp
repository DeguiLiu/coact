/*
 * Deterministic target output for the coact Renode harness.
 *
 * Writes to USART2 (0x40004400), whose DR is backed by a Renode file backend.
 * No libc stdio, no allocation, no interrupts.
 */
#pragma once

#include <cstdint>

namespace harness {

namespace detail {

constexpr uintptr_t kUart2Base = 0x40004400UL;

inline volatile uint32_t* uart_reg(uint32_t index) noexcept {
  return reinterpret_cast<volatile uint32_t*>(kUart2Base) + index;
}

}  // namespace detail

inline void uart_init() noexcept {
  /* UE | TE; the Renode model accepts writes to DR without a baud match. */
  *detail::uart_reg(3U) = 0x2008U;
}

inline void putc(char c) noexcept {
  /* SR.TXE (bit 7) before every byte keeps the backend ordering explicit. */
  while (0U == (*detail::uart_reg(0U) & 0x80U)) {}
  *detail::uart_reg(1U) = static_cast<uint32_t>(static_cast<uint8_t>(c));
}

inline void puts(const char* s) noexcept {
  while ('\0' != *s) {
    putc(*s);
    ++s;
  }
}

inline void put_u64(uint64_t value) noexcept {
  char buf[21];
  uint32_t n = 0U;
  if (0U == value) {
    putc('0');
    return;
  }
  while (0U != value) {
    buf[n] = static_cast<char>('0' + static_cast<char>(value % 10U));
    value /= 10U;
    ++n;
  }
  while (n > 0U) {
    --n;
    putc(buf[n]);
  }
}

inline void put_i64(int64_t value) noexcept {
  if (value < 0) {
    putc('-');
    put_u64(static_cast<uint64_t>(0) - static_cast<uint64_t>(value));
  } else {
    put_u64(static_cast<uint64_t>(value));
  }
}

inline void put_hex32(uint32_t value) noexcept {
  static const char* const kDigits = "0123456789abcdef";
  harness::puts("0x");
  for (int shift = 28; shift >= 0; shift -= 4) {
    putc(kDigits[(value >> shift) & 0xFU]);
  }
}

inline void newline() noexcept {
  putc('\r');
  putc('\n');
}

}  // namespace harness
