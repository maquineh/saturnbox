#include "test_support.hpp"
#include "saturn/saturn/mmio.hpp"
#include "saturn/saturn/scheduler.hpp"

namespace {
using saturn::platform::VideoRuntime;
constexpr auto regs = VideoRuntime::kVdp1Registers;
constexpr auto fb = VideoRuntime::kVdp1Framebuffer;

void configure(VideoRuntime& video) {
    REQUIRE(video.write(VideoRuntime::kVdp2Tvmd, 2, 0x8010U));
    REQUIRE(video.write(VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(video.write(VideoRuntime::kVdp2Registers + 0x0F0U, 2, 1U));
    REQUIRE(video.write(regs + 0x0AU, 2, (39U << 9U) | 239U));
}

void manualChangeIsRequestedByEachFbcrWrite() {
    VideoRuntime video;
    configure(video);
    REQUIRE(video.write(regs + 2U, 2, 3U));
    REQUIRE(video.write(fb, 2, 0x83E0U));
    video.presentFrame(); // A CPU-populated bank needs no PTMR draw.
    REQUIRE(video.lastFrame().pixels[0] == 0xFF00FF00U);
    video.presentFrame(); // The request is consumed once.
    REQUIRE(video.lastFrame().pixels[0] == 0xFF00FF00U);
    REQUIRE(video.write(fb, 2, 0x801FU));
    REQUIRE(video.write(regs + 2U, 2, 3U)); // Same value, new request.
    video.presentFrame();
    REQUIRE(video.lastFrame().pixels[0] == 0xFFFF0000U);
    REQUIRE(video.read(fb, 2) == 0x83E0U); // Change alone preserves old pixels.
}

void drawingDoesNotRequestAManualChange() {
    VideoRuntime video;
    configure(video);
    REQUIRE(video.write(regs + 2U, 2, 3U));
    REQUIRE(video.write(fb, 2, 0x83E0U));
    REQUIRE(video.write(regs + 4U, 2, 1U));
    video.presentFrame();
    REQUIRE(video.lastFrame().pixels[0] == 0xFF00FF00U);
    REQUIRE(video.write(fb, 2, 0x801FU));
    REQUIRE(video.write(regs + 4U, 2, 1U));
    video.presentFrame();
    REQUIRE(video.lastFrame().pixels[0] == 0xFF00FF00U);
}

void vblankEraseClearsTheOldDisplayBeforeChange() {
    saturn::platform::MmioInterceptor mmio;
    auto& video = mmio.video();
    configure(video);
    REQUIRE(video.write(regs + 2U, 2, 3U));
    REQUIRE(video.write(fb, 2, 0x83E0U));
    REQUIRE(video.write(regs + 4U, 2, 1U));
    video.presentFrame(); // Bank 1: green display. Bank 0: next draw.
    REQUIRE(video.write(fb, 2, 0x801FU));
    saturn::platform::VideoDiagnosticSnapshot snapshot;
    unsigned presentations{};
    video.setDiagnosticSink([&](const auto& value) { snapshot = value; });
    video.setFrameSink([&](const auto&) { ++presentations; });
    saturn::sir::Module module;
    auto& block = module.createBlock(0x06010000U, "blank-erase");
    saturn::sir::MachineState state;
    mmio.attachMachineState(state);
    saturn::platform::ScuEventScheduler scheduler(263U);
    std::size_t step{};
    while (scheduler.stats().vblankEvents == 0U)
        static_cast<void>(scheduler.beforeBlock(block, state, step++, mmio));
    REQUIRE(presentations == 0U);
    // The guest IN handler requests VBlank erase + change.
    REQUIRE(video.write(regs, 2, 8U));
    REQUIRE(video.write(regs + 2U, 2, 3U));
    while (scheduler.stats().vblankOutEvents == 0U)
        static_cast<void>(scheduler.beforeBlock(block, state, step++, mmio));
    REQUIRE(presentations == 1U);
    REQUIRE(snapshot.vdp1DisplayBank == 0U);
    REQUIRE(snapshot.vdp1Banks[0].pixels[0] == 0xFFFF0000U);
    REQUIRE(snapshot.vdp1Banks[1].pixels[0] == 0xFF000000U);
    // The OUT handler clears VBE, before the next IN boundary can sample it.
    REQUIRE(video.write(regs, 2, 0U));
    while (scheduler.stats().vblankOutEvents < 2U)
        static_cast<void>(scheduler.beforeBlock(block, state, step++, mmio));
    REQUIRE(presentations == 2U);
    REQUIRE(video.lastFrame().pixels[0] == 0xFFFF0000U);
}

void manualErasePreservesPixelsOutsideItsWindow() {
    VideoRuntime video;
    configure(video);
    constexpr auto inside = (20U * VideoRuntime::kVdp1FramebufferWidth + 8U) * 2U;
    REQUIRE(video.write(regs + 2U, 2, 3U));
    REQUIRE(video.write(fb, 2, 0x83E0U));
    REQUIRE(video.write(fb + inside, 2, 0x83E0U));
    video.presentFrame();
    REQUIRE(video.write(regs + 8U, 2, (1U << 9U) | 20U));
    REQUIRE(video.write(regs + 10U, 2, (1U << 9U) | 20U));
    REQUIRE(video.write(regs + 6U, 2, 0U));
    REQUIRE(video.write(regs + 2U, 2, 2U));
    video.presentFrame();
    REQUIRE(video.lastFrame().pixels[0] == 0xFF00FF00U);
    REQUIRE(video.lastFrame().pixels[20U * 320U + 8U] == 0xFF000000U);
    // Erase-only must not swap. Its old draw bank is still zero.
    REQUIRE(video.read(fb, 2) == 0U);
    REQUIRE(video.write(regs + 6U, 2, 0x801FU));
    video.presentFrame(); // FBCR=2 is a request, not continuous erase.
    REQUIRE(video.lastFrame().pixels[20U * 320U + 8U] == 0xFF000000U);
}

void vbePulseIsLatchedAndErasesUpperRowsAcrossBothBanks() {
    VideoRuntime video;
    configure(video);
    REQUIRE(video.write(regs + 2U, 2, 3U));
    REQUIRE(video.write(fb, 2, 0x83E0U));
    video.presentFrame();
    for (unsigned field = 0; field < 2U; ++field) {
        video.onVBlankIn();
        REQUIRE(video.write(regs, 2, 8U));
        REQUIRE(video.write(regs + 2U, 2, 3U));
        REQUIRE(video.write(regs, 2, 0U)); // An initiated erase is not lost.
        video.onVBlankOut();
        REQUIRE(video.lastFrame().pixels[0] == 0xFF000000U);
        REQUIRE(video.read(fb, 2) == 0U);
    }
}

void automaticDrawingPreparesTheNewBackBank() {
    VideoRuntime video;
    configure(video);
    REQUIRE(video.write(regs + 2U, 2, 0U));
    REQUIRE(video.write(fb, 2, 0x83E0U));
    // A red polygon at (0,0) will be drawn after the first field change.
    REQUIRE(video.write(VideoRuntime::kVdp1Base, 2, 4U));
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 6U, 2, 0x801FU));
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 0x10U, 2, 1U));
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 0x14U, 2, 1U));
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 0x16U, 2, 1U));
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 0x1AU, 2, 1U));
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 0x20U, 2, 0x8000U));
    REQUIRE(video.write(regs + 4U, 2, 2U));
    video.onVBlankIn();
    video.onVBlankOut();
    REQUIRE(video.lastFrame().pixels[0] == 0xFF00FF00U);
    REQUIRE(video.read(fb, 2) == 0x801FU);
    // Blue belongs to the following back bank, not the frame now on display.
    REQUIRE(video.write(VideoRuntime::kVdp1Base + 6U, 2, 0xFC00U));
    video.onVBlankIn();
    video.onVBlankOut();
    REQUIRE(video.lastFrame().pixels[0] == 0xFFFF0000U);
    REQUIRE(video.read(fb, 2) == 0xFC00U);
}

void automaticTriggerWaitsForAManualBankChange() {
    VideoRuntime video;
    configure(video);
    video.setVdp1CommandTraceEnabled(true);
    REQUIRE(video.write(VideoRuntime::kVdp1Base, 2, 0x8000U));
    REQUIRE(video.write(regs + 2U, 2, 3U));
    REQUIRE(video.write(regs + 4U, 2, 2U));
    video.onVBlankIn();
    video.onVBlankOut();
    REQUIRE(video.takeVdp1DrawEnd());
    video.onVBlankIn();
    video.onVBlankOut();
    REQUIRE(!video.takeVdp1DrawEnd());
    saturn::platform::VideoDiagnosticSnapshot snapshot;
    video.setDiagnosticSink([&](const auto& value) { snapshot = value; });
    REQUIRE(video.write(regs + 2U, 2, 3U));
    video.onVBlankIn();
    video.onVBlankOut();
    REQUIRE(video.takeVdp1DrawEnd());
    REQUIRE(snapshot.vdp1CommandHistory.size() == 2U);
    REQUIRE(snapshot.vdp1CommandHistory[0].wasPresented);
    REQUIRE(snapshot.vdp1CommandHistory[0].presentedFrame == snapshot.composed.sequence);
}

void explicitVblankStillHasAnOutBoundary() {
    saturn::platform::MmioInterceptor mmio;
    saturn::platform::ScuEventScheduler scheduler;
    saturn::sir::MachineState state;
    saturn::sir::Module module;
    auto& block = module.createBlock(0x06010000U, "explicit-blank");
    mmio.attachMachineState(state);
    scheduler.requestVBlank();
    static_cast<void>(scheduler.beforeBlock(block, state, 0U, mmio));
    REQUIRE(mmio.video().frameCount() == 0U);
    static_cast<void>(scheduler.beforeBlock(block, state, 1U, mmio));
    REQUIRE(scheduler.stats().vblankOutEvents == 1U);
    REQUIRE(mmio.video().frameCount() == 1U);
}
} // namespace

void registerVdp1FramebufferTests(test::Registry& registry) {
    registry.add("VDP1: FBCR writes request one manual change",
                 manualChangeIsRequestedByEachFbcrWrite);
    registry.add("VDP1: drawing does not request manual change",
                 drawingDoesNotRequestAManualChange);
    registry.add("VDP1: erase during blank precedes change at OUT",
                 vblankEraseClearsTheOldDisplayBeforeChange);
    registry.add("VDP1: manual erase respects its window without swapping",
                 manualErasePreservesPixelsOutsideItsWindow);
    registry.add("VDP1: VBE pulse erases upper rows in both banks",
                 vbePulseIsLatchedAndErasesUpperRowsAcrossBothBanks);
    registry.add("VDP1: automatic draw prepares the next back bank",
                 automaticDrawingPreparesTheNewBackBank);
    registry.add("VDP1: PTMR2 waits for a manual bank change",
                 automaticTriggerWaitsForAManualBankChange);
    registry.add("VDP1: explicit VBlank includes OUT presentation",
                 explicitVblankStillHasAnOutBoundary);
}
