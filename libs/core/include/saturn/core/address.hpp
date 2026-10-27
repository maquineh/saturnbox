#pragma once

#include <cstdint>

namespace saturn::core {

using Address = std::uint32_t;

[[nodiscard]] constexpr bool isInstructionAligned(Address address) noexcept {
    return (address & 1U) == 0;
}

} // namespace saturn::core
