// coact BitFieldView: compile-time view of one bit-field inside a uint32
// register. Zero runtime overhead: mask and shift are constexpr; read/write
// compile to the same instructions as hand-written RMW code.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace coact {

// View of the bit-field [Offset, Offset+Width) of a 32-bit register.
// Reg is a tag type only — each view belongs to one register block and
// carries no storage. Every field owns an INDEPENDENT read-modify-write:
// write() touches only this field's bits, so updating one field can never
// corrupt its neighbors. That is the property the "register = one uint32
// scalar" modeling loses; it is the bit-field structure the SOUT opcode /
// mode contract inversion class of hardware faults depended on.
template <typename Reg, std::uint32_t Offset, std::uint32_t Width>
struct BitFieldView {
    static_assert(0U < Width,
                  "BitFieldView width must be at least 1 bit");
    static_assert(Offset + Width <= 32U,
                  "BitFieldView exceeds the 32-bit register word");

    using reg_type = Reg;

    static constexpr std::uint32_t offset = Offset;
    static constexpr std::uint32_t width  = Width;
    // Width == 32 needs a 64-bit shift: 1U << 32 would be UB.
    static constexpr std::uint32_t mask =
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(1U) << Width) - 1U) << Offset;

    // Field value, shifted down to bit 0 (ready for direct compare).
    [[nodiscard]] static constexpr std::uint32_t read(std::uint32_t reg) noexcept
    {
        return static_cast<std::uint32_t>((reg & mask) >> Offset);
    }

    // Read-modify-write of this field only; neighbors keep their bits.
    // The shifted value is masked against `mask` so a caller passing a
    // wider-than-Width value cannot leak bits into adjacent fields.
    static constexpr void write(std::uint32_t& reg, std::uint32_t val) noexcept
    {
        reg = static_cast<std::uint32_t>((reg & ~mask)
                                         | ((val << Offset) & mask));
    }
};

// Compile-time check that two field views in the same register never overlap.
// Catches the most common BitFieldView mistake at template instantiation
// rather than at the first write that corrupts its neighbor.
template <typename A, typename B>
[[nodiscard]] static constexpr bool fields_disjoint() noexcept
{
    return (A::mask & B::mask) == 0U;
}

}  // namespace coact
