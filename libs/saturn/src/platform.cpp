#include "saturn/saturn/platform.hpp"

namespace saturn::platform {

core::Address canonicalizeRamAddress(core::Address address) noexcept {
    const auto segment = address & 0xE0000000U;
    const auto physical = address & 0x1FFFFFFFU;
    if (segment == 0x20000000U &&
        ((physical >= 0x00200000U && physical <= 0x002FFFFFU) ||
         (physical >= 0x06000000U && physical <= 0x060FFFFFU))) {
        return physical;
    }
    if (segment >= 0x80000000U && segment <= 0xC0000000U &&
        physical >= 0x06000000U && physical <= 0x060FFFFFU) {
        return physical;
    }
    return address;
}

AddressSpace classify(core::Address address) noexcept {
    const auto ram = canonicalizeRamAddress(address);
    if (ram >= 0x00200000U && ram <= 0x002FFFFFU) return AddressSpace::LowWorkRam;
    if (ram >= 0x06000000U && ram <= 0x060FFFFFU) return AddressSpace::HighWorkRam;
    if (address >= 0x20100000U && address <= 0x2010007FU) return AddressSpace::Smpc;
    if (address >= 0x05C00000U && address <= 0x05D7FFFFU) return AddressSpace::Vdp1;
    // 0x25xxxxxx is the SH-2 cache-bypassing alias of the 0x05xxxxxx
    // peripheral window. Games normally use this alias for MMIO.
    if (address >= 0x25C00000U && address <= 0x25D7FFFFU) return AddressSpace::Vdp1;
    if (address >= 0x05E00000U && address <= 0x05FDFFFFU) return AddressSpace::Vdp2;
    if (address >= 0x25E00000U && address <= 0x25FDFFFFU) return AddressSpace::Vdp2;
    if ((address >= 0x05FE0000U && address <= 0x05FEFFFFU) ||
        (address >= 0x25FE0000U && address <= 0x25FEFFFFU)) {
        return AddressSpace::Scu;
    }
    if (address >= 0x05A00000U && address <= 0x05BFFFFFU) return AddressSpace::Scsp;
    if (address >= 0x25A00000U && address <= 0x25BFFFFFU) return AddressSpace::Scsp;
    if (address >= 0x05800000U && address <= 0x058FFFFFU) return AddressSpace::CdBlock;
    if (address >= 0x25800000U && address <= 0x258FFFFFU) return AddressSpace::CdBlock;
    return AddressSpace::Unknown;
}

std::string_view name(AddressSpace space) noexcept {
    switch (space) {
    case AddressSpace::Unknown: return "unknown";
    case AddressSpace::Bios: return "bios";
    case AddressSpace::LowWorkRam: return "low-work-ram";
    case AddressSpace::HighWorkRam: return "high-work-ram";
    case AddressSpace::Smpc: return "smpc";
    case AddressSpace::Vdp1: return "vdp1";
    case AddressSpace::Vdp2: return "vdp2";
    case AddressSpace::Scu: return "scu";
    case AddressSpace::Scsp: return "scsp";
    case AddressSpace::CdBlock: return "cd-block";
    }
    return "unknown";
}

} // namespace saturn::platform
