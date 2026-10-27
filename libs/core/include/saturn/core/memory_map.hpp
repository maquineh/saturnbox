#pragma once

#include "saturn/core/address.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

namespace saturn::core {

// A non-overlapping collection of byte segments in the SH-2 address space.
//
// A Saturn boot session is not one contiguous file: the IP/AIP, the first
// executable and later DMA buffers occupy different addresses. MemoryMap keeps
// those regions sparse while presenting the same big-endian read interface as
// the original single-file image.
struct MemorySegment {
    Address base{};
    std::vector<std::byte> bytes;
};

class MemoryMap {
public:
    MemoryMap() = default;
    MemoryMap(Address base, std::vector<std::byte> bytes);

    [[nodiscard]] static std::optional<MemoryMap> fromFile(
        const std::filesystem::path& path,
        Address base);

    // Adds a region if it fits in the 32-bit address space and does not
    // overlap an existing region. Empty regions are accepted as no-ops.
    [[nodiscard]] bool addSegment(Address base, std::vector<std::byte> bytes);

    [[nodiscard]] Address base() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t segmentCount() const noexcept;
    [[nodiscard]] bool contains(Address address, std::size_t length = 1) const noexcept;

    [[nodiscard]] std::optional<std::uint8_t> read8(Address address) const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> read16BE(Address address) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> read32BE(Address address) const noexcept;

    [[nodiscard]] const std::map<Address, MemorySegment>& segments() const noexcept;

private:
    Address baseHint_{};
    std::size_t byteCount_{};
    std::map<Address, MemorySegment> segments_;
};

} // namespace saturn::core
