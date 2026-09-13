// coact BitFieldView test: compile-time mask/shift boundaries, RMW
// isolation across adjacent fields, and end-to-end integration with
// PeriphRegCache's three-flag protocol (bypass / cache_only / write-through)
// through the field-level write_field / read_field entry.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>

#include "coact/bitfield.hpp"
#include "coact/reg_cache.hpp"

namespace {

using coact::BitFieldView;
using coact::BypassGuard;
using coact::fields_disjoint;
using coact::PeriphRegCache;

// ---------------------------------------------------------------------------
// Boundary fields: width 1 (a single bit), width 32 (the whole register),
// offset 0 (LSB lane), offset 28 (top-nibble lane). The mask / read /
// write formulas are exercised at every boundary the documentation names.
// ---------------------------------------------------------------------------
struct RegX {};
using XBit0    = BitFieldView<RegX, 0U, 1U>;   // offset 0, width 1 (LSB)
using XBit28   = BitFieldView<RegX, 28U, 1U>;  // offset 28, width 1
using XFull32  = BitFieldView<RegX, 0U, 32U>;  // width 32 (full register)
using XHigh4   = BitFieldView<RegX, 28U, 4U>;  // offset 28, width 4
using XMid4    = BitFieldView<RegX, 4U, 4U>;   // offset 4, width 4
using XWhole   = BitFieldView<RegX, 0U, 32U>;  // alias for full-width

// ---------------------------------------------------------------------------
// Toy register block for the PeriphRegCache integration test: two words,
// each shared by fields that must not disturb one another.
//   word 0: [0] enable, [4:7] opcode, [8:9] mode
//   word 1: [0] bypass, [1:2] magx
// ---------------------------------------------------------------------------
struct Word0Tag {};
struct Word1Tag {};

using FEnable = BitFieldView<Word0Tag, 0U, 1U>;
using FOpcode = BitFieldView<Word0Tag, 4U, 4U>;
using FMode   = BitFieldView<Word0Tag, 8U, 2U>;
using FBypass = BitFieldView<Word1Tag, 0U, 1U>;
using FMagx   = BitFieldView<Word1Tag, 1U, 2U>;

static_assert(fields_disjoint<FEnable, FOpcode>(), "enable/opcode overlap");
static_assert(fields_disjoint<FEnable, FMode>(),   "enable/mode overlap");
static_assert(fields_disjoint<FOpcode, FMode>(),   "opcode/mode overlap");
static_assert(fields_disjoint<FBypass, FMagx>(),   "bypass/magx overlap");

inline constexpr std::uint32_t kOpcodeFrame = 0x2U;
inline constexpr std::uint32_t kOpcodeRecfg = 0xAU;
inline constexpr std::uint32_t kModeActive  = 0x2U;
inline constexpr std::uint32_t kEnableOn    = 0x1U;

inline constexpr std::uint16_t kRegWord0 = 6U;
inline constexpr std::uint16_t kRegWord1 = 7U;

// ---------------------------------------------------------------------------
// Test: mask / shift boundaries compile to the right constants and read /
// write the expected lanes at offset 0, offset 28, width 1, width 32.
// ---------------------------------------------------------------------------
COACT_TEST(bitfield_mask_boundaries)
{
    // Width 1 lanes: mask is a single bit at the named offset.
    static_assert(XBit0::offset == 0U && XBit0::width == 1U
                  && XBit0::mask == 0x00000001U,
                  "bit0 mask must be 0x1");
    static_assert(XBit28::offset == 28U && XBit28::width == 1U
                  && XBit28::mask == 0x10000000U,
                  "bit28 mask must be 0x10000000");

    // Width 32 / offset 0: the whole register.
    static_assert(XFull32::mask == 0xFFFFFFFFU,
                  "full-width mask must be 0xFFFFFFFF");

    // Width 4 at offset 28: top nibble (4 bits).
    static_assert(XHigh4::mask == 0xF0000000U,
                  "high-4 mask must be 0xF0000000");

    // read() returns the field value shifted down to bit 0.
    constexpr std::uint32_t r1 = 0x10000000U;
    CHECK_EQ(XBit28::read(r1), 1U);
    constexpr std::uint32_t r2 = 0xF0000000U;
    CHECK_EQ(XHigh4::read(r2), 0xFU);
    constexpr std::uint32_t r3 = 0xDEADBEEFU;
    CHECK_EQ(XBit0::read(r3), 1U);
    CHECK_EQ(XBit28::read(r3), 1U);
    CHECK_EQ(XMid4::read(r3), 0xEU);
    CHECK_EQ(XHigh4::read(r3), 0xDU);

    // Width 32: read returns the whole register (read == identity).
    CHECK_EQ(XFull32::read(0xCAFEBABEU), 0xCAFEBABEU);
}

// ---------------------------------------------------------------------------
// Test: write() moves ONLY the named field; every adjacent lane keeps its
// value. The RMW isolation property is the whole point of the abstraction.
// ---------------------------------------------------------------------------
COACT_TEST(bitfield_rmw_isolation)
{
    // Neighbors XMid4 (bits 4..7) and XHigh4 (bits 28..31) are disjoint.
    static_assert(fields_disjoint<XMid4, XHigh4>(), "neighbors must be disjoint");
    static_assert(fields_disjoint<XBit0, XMid4>(),   "neighbors must be disjoint");
    static_assert(fields_disjoint<XBit0, XHigh4>(),  "neighbors must be disjoint");

    // XMid4 write: only bits 4..7 change; bits 0, 8..27, 28..31 stay.
    std::uint32_t r = 0xFFFF'FFFFU;
    XMid4::write(r, 0xAU);                 // set bits 4..7 to 0xA
    CHECK_EQ(r, 0xFFFF'FFAFU);              // only bits 4..7 cleared-to-A
    CHECK_EQ(XMid4::read(r), 0xAU);
    CHECK_EQ(XBit0::read(r), 1U);          // bit 0 untouched
    CHECK_EQ(XHigh4::read(r), 0xFU);       // high nibble untouched

    // XHigh4 write: only bits 28..31 change.
    XHigh4::write(r, 0x5U);
    CHECK_EQ(r, 0x5FFF'FFAFU);
    CHECK_EQ(XHigh4::read(r), 0x5U);
    CHECK_EQ(XMid4::read(r), 0xAU);        // mid nibble untouched
    CHECK_EQ(XBit0::read(r), 1U);

    // XBit28 write (single bit at offset 28): only bit 28 flips.
    std::uint32_t r2 = 0x0FFF'FFAFU;
    XBit28::write(r2, 1U);
    CHECK_EQ(r2, 0x1FFF'FFAFU);
    XBit28::write(r2, 0U);
    CHECK_EQ(r2, 0x0FFF'FFAFU);

    // Full-width write: value = caller value (the whole register).
    std::uint32_t r3 = 0x0U;
    XWhole::write(r3, 0x12345678U);
    CHECK_EQ(r3, 0x12345678U);
    CHECK_EQ(XFull32::read(r3), 0x12345678U);

    // Mask guard: writing a value wider than the field cannot leak bits
    // into adjacent lanes.
    std::uint32_t r4 = 0x0U;
    XMid4::write(r4, 0xFFU);               // caller passes 0xFF, Width=4
    CHECK_EQ(r4, 0xF0U);                   // only low 4 of mid nibble set
    CHECK_EQ(XBit0::read(r4), 0U);         // bit 0 untouched
}

// ---------------------------------------------------------------------------
// Test: PeriphRegCache field-level entry preserves the three-flag protocol
// AND moves only the field's bits in the backing uint32.
// ---------------------------------------------------------------------------
COACT_TEST(bitfield_cache_integration)
{
    PeriphRegCache regs{};
    // Default write-through: both copies carry the field RMW; adjacent
    // lanes in word 0 (enable [0], mode [8:9]) untouched.
    regs.write_field<FEnable>(kRegWord0, kEnableOn);
    regs.write_field<FMode>(kRegWord0, kModeActive);
    regs.write_field<FOpcode>(kRegWord0, kOpcodeFrame);
    regs.write_field<FBypass>(kRegWord1, 0U);
    regs.write_field<FMagx>(kRegWord1, 1U);

    // Snapshot neighbors.
    const std::uint32_t mode_before   = regs.read_field<FMode>(kRegWord0);
    const std::uint32_t enable_before = regs.read_field<FEnable>(kRegWord0);
    const std::uint32_t bypass_before = regs.read_field<FBypass>(kRegWord1);

    // The isolation RMWs through the field-level entry.
    regs.write_field<FOpcode>(kRegWord0, kOpcodeRecfg);
    regs.write_field<FMagx>(kRegWord1, 2U);

    // Single-entry coherency: shadow == hardware for both toy registers
    // (write-through path keeps both copies equal on every write).
    CHECK_EQ(regs.st.shadow[kRegWord0], regs.st.hardware[kRegWord0]);
    CHECK_EQ(regs.st.shadow[kRegWord1], regs.st.hardware[kRegWord1]);
    // Neighbors untouched in BOTH copies.
    CHECK_EQ(FMode::read(regs.st.shadow[kRegWord0]), mode_before);
    CHECK_EQ(FMode::read(regs.st.hardware[kRegWord0]), mode_before);
    CHECK_EQ(FEnable::read(regs.st.shadow[kRegWord0]), enable_before);
    CHECK_EQ(FEnable::read(regs.st.hardware[kRegWord0]), enable_before);
    CHECK_EQ(FBypass::read(regs.st.shadow[kRegWord1]), bypass_before);
    CHECK_EQ(FBypass::read(regs.st.hardware[kRegWord1]), bypass_before);
    // The moved fields moved to the new values.
    CHECK_EQ(FOpcode::read(regs.st.shadow[kRegWord0]), kOpcodeRecfg);
    CHECK_EQ(FMagx::read(regs.st.shadow[kRegWord1]), 2U);

    // cache_only path: write_field freezes hardware, marks dirty, commits
    // on sync. The same neighbor-isolation guarantee must hold.
    regs.cache_only = true;
    regs.write_field<FOpcode>(kRegWord0, kOpcodeFrame); // dirty
    CHECK(regs.cache_dirty);
    CHECK(regs.dirty_regs[kRegWord0]);
    CHECK_EQ(FMode::read(regs.st.shadow[kRegWord0]), mode_before);
    // sync() refuses while still cache_only.
    CHECK_EQ(regs.sync(), false);
    regs.cache_only = false;
    CHECK_EQ(regs.sync(), true);
    // After sync the dirty word landed in hardware and neighbors survive.
    CHECK_EQ(FOpcode::read(regs.st.hardware[kRegWord0]), kOpcodeFrame);
    CHECK_EQ(FMode::read(regs.st.hardware[kRegWord0]), mode_before);
    CHECK(!regs.cache_dirty);
    CHECK(!regs.dirty_regs[kRegWord0]);

    // cache_bypass path: write_field touches hardware only; resync on
    // guard exit must capture the field-RMWed value into the shadow.
    {
        PeriphRegCache regs2{};
        regs2.write_field<FEnable>(kRegWord0, kEnableOn);
        regs2.write_field<FMode>(kRegWord0, kModeActive);
        regs2.write_field<FOpcode>(kRegWord0, kOpcodeFrame);
        const std::uint32_t shadow_pre = regs2.st.shadow[kRegWord0];
        {
            BypassGuard guard(regs2);                    // flips cache_bypass
            regs2.write_field<FOpcode>(kRegWord0, kOpcodeRecfg);
            // During bypass, shadow unchanged, hardware moved.
            CHECK_EQ(regs2.st.shadow[kRegWord0], shadow_pre);
            CHECK_EQ(FOpcode::read(regs2.st.hardware[kRegWord0]),
                      kOpcodeRecfg);
        }                                               // resync on exit
        // After resync, shadow agrees with hardware again.
        CHECK_EQ(regs2.st.shadow[kRegWord0],
                  regs2.st.hardware[kRegWord0]);
        CHECK_EQ(FOpcode::read(regs2.st.shadow[kRegWord0]),
                  kOpcodeRecfg);
        CHECK_EQ(FMode::read(regs2.st.shadow[kRegWord0]), kModeActive);
        CHECK_EQ(FEnable::read(regs2.st.shadow[kRegWord0]), kEnableOn);
    }
}

}  // namespace

COACT_TEST_MAIN()
