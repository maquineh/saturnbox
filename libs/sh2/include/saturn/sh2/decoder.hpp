#pragma once

#include "saturn/sh2/instruction.hpp"

#include <cstdint>

namespace saturn::sh2 {

struct DecodeResult {
    DecodedInstruction instruction;

    [[nodiscard]] bool supported() const noexcept { return instruction.valid; }
};

class Decoder {
public:
    [[nodiscard]] DecodeResult decode(Address pc, std::uint16_t word) const;
};

template <int Bits>
[[nodiscard]] constexpr std::int32_t signExtend(std::uint32_t value) noexcept {
    static_assert(Bits > 0 && Bits <= 32);
    constexpr auto sign = std::uint32_t{1} << (Bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

[[nodiscard]] constexpr Address pcRelativeTarget(
    Address pc,
    std::int32_t displacement) noexcept {
    return static_cast<Address>(static_cast<std::int64_t>(pc) + 4 +
                                static_cast<std::int64_t>(displacement) * 2);
}

[[nodiscard]] constexpr Address pcRelativeWordTarget(
    Address pc,
    std::int32_t displacement) noexcept {
    const auto alignedPc = static_cast<std::uint32_t>((pc + 4U) & ~3U);
    return static_cast<Address>(static_cast<std::int64_t>(alignedPc) +
                                static_cast<std::int64_t>(displacement) * 4);
}

} // namespace saturn::sh2
