// Compact log encoding: field preservation, bounded output and deferred sink.
// SPDX-License-Identifier: MIT
#include "coact/diag/log.hpp"
#include "test/test_harness.hpp"

#include <array>
#include <string>

namespace {

using coact::diag::CompactHexPolicy;
using coact::diag::LogLane;
using coact::diag::LogLevel;
using coact::diag::LogRecord;

uint32_t decode_hex(const char* text, uint16_t digits) {
  uint32_t value = 0U;
  for (uint16_t i = 0U; i < digits; ++i) {
    const char ch = text[i];
    const uint32_t nibble = (ch <= '9') ? static_cast<uint32_t>(ch - '0') : static_cast<uint32_t>(ch - 'A' + 10);
    value = (value << 4U) | nibble;
  }
  return value;
}

struct Hooks {
  uint32_t writes = 0U;
  uint32_t task_signals = 0U;
  uint32_t isr_signals = 0U;
};

uint32_t read_counter(void*) noexcept {
  return 0x12345678U;
}

bool write_sink(void* context, std::string_view) noexcept {
  ++static_cast<Hooks*>(context)->writes;
  return true;
}

void signal_task(void* context) noexcept {
  ++static_cast<Hooks*>(context)->task_signals;
}

void signal_isr(void* context) noexcept {
  ++static_cast<Hooks*>(context)->isr_signals;
}

}  // namespace

COACT_TEST(compact_log_golden_record) {
  const LogRecord record{0x12345678U, 0xABCDU, 0x0123U, 0U, 1U, 0x80000000U, 0xFFFFFFFFU};
  char buffer[CompactHexPolicy::kBufferSize]{};
  const auto result = CompactHexPolicy::render(record, buffer, sizeof(buffer));
  CHECK(result.handled);
  CHECK(!result.truncated);
  CHECK_EQ(result.length, CompactHexPolicy::kEncodedLength);
  CHECK_EQ(std::string(buffer), std::string("@12345678:ABCD:0123:00000000:00000001:80000000:FFFFFFFF\n"));
}

COACT_TEST(compact_log_round_trip_all_fields) {
  const std::array<uint32_t, 6U> values{{0U, 1U, 0xFFFFU, 0x80000000U, 0xABCDEF01U, 0xFFFFFFFFU}};
  for (uint32_t value : values) {
    // Unknown ids are encoded without a catalog lookup or loss of payload.
    const LogRecord record{value,      static_cast<uint16_t>(value), static_cast<uint16_t>(~value),
                           value,      value ^ 0xFFFFFFFFU,          value >> 1U,
                           value << 1U};
    char buffer[CompactHexPolicy::kBufferSize]{};
    const auto result = CompactHexPolicy::render(record, buffer, sizeof(buffer));
    REQUIRE(!result.truncated);
    CHECK_EQ(decode_hex(buffer + 1U, 8U), record.counter);
    CHECK_EQ(decode_hex(buffer + 10U, 4U), record.event_id);
    CHECK_EQ(decode_hex(buffer + 15U, 4U), record.source_id);
    CHECK_EQ(decode_hex(buffer + 20U, 8U), record.arg0);
    CHECK_EQ(decode_hex(buffer + 29U, 8U), record.arg1);
    CHECK_EQ(decode_hex(buffer + 38U, 8U), record.arg2);
    CHECK_EQ(decode_hex(buffer + 47U, 8U), record.arg3);
    CHECK_EQ(buffer[55U], '\n');
    CHECK_EQ(buffer[56U], '\0');
  }
}

COACT_TEST(compact_log_rejects_small_buffers_without_partial_lines) {
  const LogRecord record{};
  for (uint16_t capacity = 0U; capacity < CompactHexPolicy::kBufferSize; ++capacity) {
    std::array<char, CompactHexPolicy::kBufferSize + 2U> buffer{};
    buffer.fill('X');
    const auto result = CompactHexPolicy::render(record, buffer.data() + 1U, capacity);
    CHECK(result.handled);
    CHECK(result.truncated);
    CHECK_EQ(result.length, uint16_t(0U));
    CHECK_EQ(buffer[0U], 'X');
    CHECK_EQ(buffer[static_cast<size_t>(capacity) + 1U], 'X');
    if (capacity > 0U) {
      CHECK_EQ(buffer[1U], '\0');
    }
  }
  const auto result = CompactHexPolicy::render(record, nullptr, CompactHexPolicy::kBufferSize);
  CHECK(result.truncated);
  CHECK_EQ(result.length, uint16_t(0U));
}

COACT_TEST(compact_log_exact_buffer_preserves_canaries) {
  const LogRecord record{};
  std::array<char, CompactHexPolicy::kBufferSize + 2U> buffer{};
  buffer.fill('X');
  const auto result = CompactHexPolicy::render(record, buffer.data() + 1U, CompactHexPolicy::kBufferSize);
  CHECK(!result.truncated);
  CHECK_EQ(buffer.front(), 'X');
  CHECK_EQ(buffer.back(), 'X');
  CHECK_EQ(buffer[CompactHexPolicy::kBufferSize], '\0');
}

COACT_TEST(compact_log_producers_defer_output_and_account_for_overflow) {
  coact::diag::Logger<2U, 1U, 100U, 100U> logger;
  coact::SpinCriticalSection gate;
  Hooks hooks{};
  const coact::diag::LogClockOps clock{read_counter, 1000U, nullptr};
  const coact::diag::LogSinkOps sink{write_sink, &hooks};
  const coact::diag::LogWakeOps wake{signal_task, signal_isr, nullptr, nullptr, &hooks};
  REQUIRE(logger.bind(clock, sink, wake, coact::make_spin_critical_section(gate)));
  logger.record<LogLevel::kInfo, 2U>(1U, 7U);
  logger.record<LogLevel::kInfo, 2U>(1U, 8U);
  logger.record<LogLevel::kInfo, 2U>(1U, 9U);
  logger.record_from_isr<LogLevel::kError, 3U>(2U, 0x82U, 0x20020000U);
  logger.record_from_isr<LogLevel::kError, 3U>(2U, 0x83U, 0x20020004U);
  CHECK_EQ(hooks.writes, uint32_t(0U));
  CHECK_EQ(hooks.task_signals, uint32_t(2U));
  CHECK_EQ(hooks.isr_signals, uint32_t(1U));

  LogRecord record{};
  REQUIRE(logger.pop_from_lane(LogLane::kCritical, record));
  CHECK_EQ(record.event_id, uint16_t(3U));
  CHECK_EQ(record.arg0, uint32_t(0x82U));
  CHECK_EQ(record.arg1, uint32_t(0x20020000U));
  char buffer[CompactHexPolicy::kBufferSize]{};
  const auto result = CompactHexPolicy::render(record, buffer, sizeof(buffer));
  REQUIRE(!result.truncated);
  CHECK(sink.write(sink.context, std::string_view(buffer, result.length)));
  CHECK_EQ(hooks.writes, uint32_t(1U));

  REQUIRE(logger.pop_from_lane(LogLane::kNormal, record));
  CHECK_EQ(record.arg0, uint32_t(7U));
  REQUIRE(logger.pop_from_lane(LogLane::kNormal, record));
  CHECK_EQ(record.arg0, uint32_t(8U));
  CHECK(!logger.pop_from_lane(LogLane::kNormal, record));
  const auto stats = logger.snapshot();
  CHECK_EQ(stats.accepted_normal, uint32_t(2U));
  CHECK_EQ(stats.dropped_at_enqueue_normal, uint32_t(1U));
  CHECK_EQ(stats.accepted_critical, uint32_t(1U));
  CHECK_EQ(stats.dropped_at_enqueue_critical, uint32_t(1U));
  CHECK_EQ(stats.drained_normal, stats.accepted_normal);
  CHECK_EQ(stats.drained_critical, stats.accepted_critical);
}

COACT_TEST_MAIN()
