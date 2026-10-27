#pragma once

#include "saturn/core/address.hpp"

#include <string_view>

namespace saturn::platform {

// Names only: M0 must not implement a Saturn runtime or hardware emulation.
enum class AddressSpace {
    Unknown,
    Bios,
    LowWorkRam,
    HighWorkRam,
    Smpc,
    Vdp1,
    Vdp2,
    Scu,
    Scsp,
    CdBlock,
};

[[nodiscard]] AddressSpace classify(core::Address address) noexcept;
// Resolve cache-through work RAM and the high-RAM aliases used by DMA
// descriptors. Cache arrays and on-chip register addresses remain distinct.
[[nodiscard]] core::Address canonicalizeRamAddress(core::Address address) noexcept;
[[nodiscard]] std::string_view name(AddressSpace space) noexcept;

} // namespace saturn::platform
