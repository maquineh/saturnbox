#include "test_support.hpp"

#include "saturn/sh2/cfg.hpp"

#include <vector>

namespace {

using namespace saturn;

void mapsSeparatedBootRegions() {
    core::MemoryMap map;
    REQUIRE(map.addSegment(0x100U, {std::byte{0xA0}, std::byte{0x01},
                                    std::byte{0x00}, std::byte{0x09}}));
    REQUIRE(map.addSegment(0x200U, {std::byte{0xC0}, std::byte{0x12}}));
    REQUIRE(map.segmentCount() == 2);
    REQUIRE(map.size() == 6);
    REQUIRE(map.contains(0x100U, 4));
    REQUIRE(!map.contains(0x102U, 3));
    REQUIRE(map.read16BE(0x200U) == 0xC012U);
    REQUIRE(!map.addSegment(0x102U, {std::byte{0x00}, std::byte{0x09}}));
}

void cfgFollowsBranchAcrossSegments() {
    core::MemoryMap map;
    // BRA 0x200; NOP. The target is deliberately in a separate region.
    REQUIRE(map.addSegment(0x100U, {std::byte{0xA0}, std::byte{0x7E},
                                    std::byte{0x00}, std::byte{0x09}}));
    REQUIRE(map.addSegment(0x200U, {std::byte{0xC0}, std::byte{0x12}}));

    const auto cfg = sh2::BlockBuilder{}.build(
        map, std::vector<core::Address>{0x100U});
    REQUIRE(cfg.blockAt(0x100U) != nullptr);
    REQUIRE(cfg.blockAt(0x200U) != nullptr);
    REQUIRE(cfg.blockAt(0x200U)->terminator.has_value());
    REQUIRE(cfg.blockAt(0x200U)->terminator->transfer.address == 0x200U);
}

} // namespace

void registerMemoryMapTests(test::Registry& registry) {
    registry.add("core: maps separated boot regions", mapsSeparatedBootRegions);
    registry.add("cfg: follows branch across memory segments", cfgFollowsBranchAcrossSegments);
}
