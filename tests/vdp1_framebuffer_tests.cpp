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

void scaledSpriteColumnsUseClippedRelativeIndices() {
    constexpr int left = -400, top = -7, width = 800, height = 253;
    constexpr int sourceWidth = 16, sourceHeight = 2;
    const auto sourceColor = [](int x, int y) {
        return static_cast<std::uint16_t>(0x8000U | (x + 1) | ((y + 1) << 5));
    };
    const auto expand5 = [](unsigned value) { return value * 255U / 31U; };
    for (unsigned zoom : {0U, 5U}) { // A/C endpoints and top-left zoom anchor.
        for (unsigned flip = 0U; flip < 4U; ++flip) {
            VideoRuntime video; // No compute renderer: exercise CPU sourceColumns.
            configure(video);
            REQUIRE(video.write(regs + 2U, 2U, 3U));
            const auto command = VideoRuntime::kVdp1Base;
            REQUIRE(video.write(command, 2U, 1U | (flip << 4U) | (zoom << 8U)));
            REQUIRE(video.write(command + 4U, 2U, 5U << 3U)); // RGB555 texture.
            REQUIRE(video.write(command + 8U, 2U, 0x100U / 8U));
            REQUIRE(video.write(command + 10U, 2U, 0x0202U)); // 16x2 source.
            REQUIRE(video.write(command + 12U, 2U, static_cast<std::uint16_t>(left)));
            REQUIRE(video.write(command + 14U, 2U, static_cast<std::uint16_t>(top)));
            if (zoom == 0U) {
                REQUIRE(video.write(command + 20U, 2U, left + width - 1));
                REQUIRE(video.write(command + 22U, 2U, top + height - 1));
            } else {
                REQUIRE(video.write(command + 16U, 2U, width - 1));
                REQUIRE(video.write(command + 18U, 2U, height - 1));
            }
            REQUIRE(video.write(command + 0x20U, 2U, 0x8000U));
            for (int y = 0; y < sourceHeight; ++y)
                for (int x = 0; x < sourceWidth; ++x)
                    REQUIRE(video.write(command + 0x100U + (y * sourceWidth + x) * 2U,
                                        2U, sourceColor(x, y)));
            REQUIRE(video.write(regs + 4U, 2U, 1U));
            // minX=400 exceeds the cache size; only x-minX is a valid index.
            // Check every visible pixel, including x=0/319 and y=0/239.
            for (unsigned y = 0U; y < 240U; ++y) {
                const auto sourceY = (static_cast<int>(y) - top) * sourceHeight / height;
                const auto sy = (flip & 2U) != 0U ? sourceHeight - 1 - sourceY : sourceY;
                for (unsigned x = 0U; x < 320U; ++x) {
                    const auto sourceX = (static_cast<int>(x) - left) * sourceWidth / width;
                    const auto sx = (flip & 1U) != 0U ? sourceWidth - 1 - sourceX : sourceX;
                    REQUIRE(video.read(fb + (y * 512U + x) * 2U, 2U) == sourceColor(sx, sy));
                }
                REQUIRE(video.read(fb + (y * 512U + 320U) * 2U, 2U) == 0U);
            }
            REQUIRE(video.read(fb + 240U * 512U * 2U, 2U) == 0U);
            video.presentFrame();
            for (unsigned y : {0U, 239U}) {
                for (unsigned x : {0U, 319U}) {
                    const auto sourceX = (static_cast<int>(x) - left) * sourceWidth / width;
                    const auto sourceY = (static_cast<int>(y) - top) * sourceHeight / height;
                    const auto sx = (flip & 1U) != 0U ? sourceWidth - 1 - sourceX : sourceX;
                    const auto sy = (flip & 2U) != 0U ? sourceHeight - 1 - sourceY : sourceY;
                    const auto expected = 0xFF000000U |
                        (expand5(static_cast<unsigned>(sx + 1)) << 16U) |
                        (expand5(static_cast<unsigned>(sy + 1)) << 8U);
                    REQUIRE(video.lastFrame().pixels[y * 320U + x] == expected);
                }
            }
        }
    }
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
    registry.add("VDP1: scaled CPU sprite caches columns relative to clipping",
                 scaledSpriteColumnsUseClippedRelativeIndices);
}
