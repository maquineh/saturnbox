#include "test_support.hpp"
#include "saturn/saturn/video.hpp"

#include <string>
#include <vector>

namespace {
using saturn::platform::VideoDebugSnapshot;
using saturn::platform::VideoRuntime;
constexpr auto regs = VideoRuntime::kVdp1Registers;
constexpr auto fb = VideoRuntime::kVdp1Framebuffer;

void debugSnapshotCopiesWithoutComposing() {
    VideoRuntime video;
    video.setDeferredComposition(true);
    video.setExecutionContext(123U, 0x06012340U, 0x06012300U);
    REQUIRE(video.write(fb, 2U, 0x83E0U));
    REQUIRE(video.write(VideoRuntime::kVdp1Base, 2U, 0x8000U));
    REQUIRE(video.write(regs + 4U, 2U, 1U));
    const auto before = video.lastFrame();
    const auto light = video.captureDebugSnapshot(false);
    const auto full = video.captureDebugSnapshot(true);
    REQUIRE(light.compositionDirty);
    REQUIRE(light.compositionCount == 0U);
    REQUIRE(full.compositionCount == 0U);
    REQUIRE(video.lastFrame().pixels == before.pixels);
    REQUIRE(light.composed.pixels == before.pixels);
    REQUIRE(light.contextStep == 123U);
    REQUIRE(light.contextBlock == 0x06012340U);
    REQUIRE(light.contextCaller == 0x06012300U);
    REQUIRE(light.framebufferBanks[0].size() == VideoRuntime::kVdp1FramebufferSize);
    REQUIRE(light.framebufferBanks[1].size() == VideoRuntime::kVdp1FramebufferSize);
    REQUIRE(light.framebufferBanks[0][0] == 0x83U);
    REQUIRE(light.vdp1Vram.empty());
    REQUIRE(light.vdp2Vram.empty());
    REQUIRE(light.cram.empty());
    REQUIRE(light.vdp1Registers.empty());
    REQUIRE(light.vdp2Registers.empty());
    REQUIRE(full.vdp1Vram.size() == VideoRuntime::kVdp1VramSize);
    REQUIRE(full.vdp2Vram.size() == VideoRuntime::kVdp2VramSize);
    REQUIRE(full.cram.size() == VideoRuntime::kVdp2CramSize);
    REQUIRE(full.vdp1Registers.size() == VideoRuntime::kRegisterSize);
    REQUIRE(full.vdp2Registers.size() == VideoRuntime::kRegisterSize);
    REQUIRE((full.vdp1Registers[0x11U] & 2U) != 0U); // Actual EDSR, not stale backing bytes.
    REQUIRE(video.write(fb, 2U, 0U));
    REQUIRE(full.framebufferBanks[0][0] == 0x83U); // Owns its memory.
}

void debugRegisterHistorySurvivesTextureTraffic() {
    VideoRuntime video;
    video.setVdp1MemoryWriteTraceEnabled(true);
    REQUIRE(video.write(regs, 2U, 0U));
    REQUIRE(video.write(regs, 2U, 8U));
    REQUIRE(video.write(regs, 2U, 0U));
    for (unsigned write = 0; write < 5000U; ++write)
        REQUIRE(video.write(VideoRuntime::kVdp1Base + 0x60000U, 2U, write));
    const auto snapshot = video.captureDebugSnapshot(false);
    REQUIRE(snapshot.vdp1MemoryWriteHistory.size() == 4096U);
    REQUIRE(snapshot.vdp1RegisterWriteHistory.size() == 3U);
    REQUIRE(snapshot.vdp1RegisterWriteHistory[0].offset == 0x100000U);
    REQUIRE(snapshot.vdp1RegisterWriteHistory[0].value == 0U);
    REQUIRE(snapshot.vdp1RegisterWriteHistory[1].value == 8U);
    REQUIRE(snapshot.vdp1RegisterWriteHistory[2].value == 0U);
}

void debugEventsExposeEachFramebufferPhase() {
    VideoRuntime video;
    video.setDeferredComposition(true);
    video.setVdp1CommandTraceEnabled(true);
    REQUIRE(video.write(regs + 2U, 2U, 3U));
    REQUIRE(video.write(fb, 2U, 0x83E0U));
    video.presentFrame(); // Green bank 1 is displayed; bank 0 is the draw side.
    REQUIRE(video.write(fb, 2U, 0x801FU));
    REQUIRE(video.write(VideoRuntime::kVdp1Base, 2U, 0x8000U));
    std::vector<std::string> events;
    std::vector<VideoDebugSnapshot> snapshots;
    video.setDebugEventSink([&](std::string_view event) {
        events.emplace_back(event);
        snapshots.push_back(video.captureDebugSnapshot(false));
    });
    video.onVBlankIn();
    REQUIRE(video.write(regs, 2U, 8U));
    REQUIRE(video.write(regs + 2U, 2U, 3U));
    REQUIRE(video.write(regs + 4U, 2U, 2U));
    video.onVBlankOut();
    const std::vector<std::string> expected{
        "before-erase", "after-erase", "after-swap", "before-draw", "after-draw"};
    REQUIRE(events == expected);
    REQUIRE(snapshots[0].framebufferBanks[1][0] == 0x83U);
    REQUIRE(snapshots[1].framebufferBanks[1][0] == 0U);
    REQUIRE(snapshots[1].vdp1DisplayBank == 1U);
    REQUIRE(snapshots[2].vdp1DisplayBank == 0U);
    REQUIRE(snapshots[2].vdp1DrawBank == 1U);
    REQUIRE(!snapshots[2].vdp1SwapPending);
    REQUIRE(snapshots[2].framebufferBanks[0][0] == 0x80U);
    REQUIRE(snapshots[4].vdp1DrawEndPending);
    REQUIRE(snapshots[4].vdp1CommandHistory.size() == 1U);
    REQUIRE(snapshots[4].vdp1RegisterWriteHistory.size() == 3U);
    for (const auto& snapshot : snapshots)
        REQUIRE(snapshot.compositionCount == snapshots[0].compositionCount);
}
} // namespace

void registerVdp1DebugTests(test::Registry& registry) {
    registry.add("VDP1 debug: snapshot owns state without composition",
                 debugSnapshotCopiesWithoutComposing);
    registry.add("VDP1 debug: register history survives texture traffic",
                 debugRegisterHistorySurvivesTextureTraffic);
    registry.add("VDP1 debug: events expose erase swap and draw phases",
                 debugEventsExposeEachFramebufferPhase);
}
