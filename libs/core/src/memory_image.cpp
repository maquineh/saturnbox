#include "saturn/core/memory_map.hpp"

#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace saturn::core {

MemoryMap::MemoryMap(Address base, std::vector<std::byte> bytes)
    : baseHint_(base) {
    (void)addSegment(base, std::move(bytes));
}

std::optional<MemoryMap> MemoryMap::fromFile(
    const std::filesystem::path& path,
    Address base) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::nullopt;
    }

    const auto end = input.tellg();
    const auto byteCount = static_cast<std::streamoff>(end);
    if (byteCount < 0 || static_cast<std::uintmax_t>(byteCount) > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(byteCount));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return std::nullopt;
        }
    }

    return MemoryMap(base, std::move(bytes));
}

bool MemoryMap::addSegment(Address base, std::vector<std::byte> bytes) {
    if (bytes.empty()) {
        if (segments_.empty()) {
            baseHint_ = base;
        }
        return true;
    }

    const auto end = static_cast<std::uint64_t>(base) + bytes.size();
    if (end > (std::uint64_t{1} << 32U)) {
        return false;
    }

    const auto next = segments_.lower_bound(base);
    if (next != segments_.end() && end > next->first) {
        return false;
    }
    if (next != segments_.begin()) {
        const auto previous = std::prev(next);
        const auto previousEnd = static_cast<std::uint64_t>(previous->first) +
                                  previous->second.bytes.size();
        if (previousEnd > base) {
            return false;
        }
    }

    if (segments_.empty()) {
        baseHint_ = base;
    }
    byteCount_ += bytes.size();
    segments_.emplace(base, MemorySegment{base, std::move(bytes)});
    return true;
}

Address MemoryMap::base() const noexcept {
    return segments_.empty() ? baseHint_ : segments_.begin()->first;
}

std::size_t MemoryMap::size() const noexcept {
    return byteCount_;
}

std::size_t MemoryMap::segmentCount() const noexcept {
    return segments_.size();
}

bool MemoryMap::contains(Address address, std::size_t length) const noexcept {
    const auto next = segments_.upper_bound(address);
    if (next == segments_.begin()) {
        return false;
    }

    const auto segment = std::prev(next);
    const auto offset = static_cast<std::uint64_t>(address) - segment->first;
    return offset <= segment->second.bytes.size() &&
           length <= segment->second.bytes.size() - static_cast<std::size_t>(offset);
}

std::optional<std::uint8_t> MemoryMap::read8(Address address) const noexcept {
    if (!contains(address)) {
        return std::nullopt;
    }
    const auto segment = std::prev(segments_.upper_bound(address));
    const auto offset = static_cast<std::size_t>(static_cast<std::uint64_t>(address) -
                                                 segment->first);
    return std::to_integer<std::uint8_t>(segment->second.bytes[offset]);
}

std::optional<std::uint16_t> MemoryMap::read16BE(Address address) const noexcept {
    if (!contains(address, 2)) {
        return std::nullopt;
    }

    const auto high = *read8(address);
    const auto low = *read8(static_cast<Address>(address + 1U));
    return static_cast<std::uint16_t>((high << 8U) | low);
}

std::optional<std::uint32_t> MemoryMap::read32BE(Address address) const noexcept {
    if (!contains(address, 4)) {
        return std::nullopt;
    }

    const auto byte0 = static_cast<std::uint32_t>(*read8(address));
    const auto byte1 = static_cast<std::uint32_t>(*read8(static_cast<Address>(address + 1U)));
    const auto byte2 = static_cast<std::uint32_t>(*read8(static_cast<Address>(address + 2U)));
    const auto byte3 = static_cast<std::uint32_t>(*read8(static_cast<Address>(address + 3U)));
    return (byte0 << 24U) | (byte1 << 16U) | (byte2 << 8U) | byte3;
}

const std::map<Address, MemorySegment>& MemoryMap::segments() const noexcept {
    return segments_;
}

} // namespace saturn::core
