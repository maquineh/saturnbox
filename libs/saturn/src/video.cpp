#include "saturn/saturn/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace saturn::platform {
namespace {

constexpr std::uint8_t kVdp2LayerNbg0 = 0U;
constexpr std::uint8_t kVdp2LayerNbg1 = 1U;
constexpr std::uint8_t kVdp2LayerNbg2 = 2U;
constexpr std::uint8_t kVdp2LayerNbg3 = 3U;
constexpr std::uint8_t kVdp2LayerBack = 4U;
constexpr std::uint8_t kVdp2LayerSprite = 5U;
constexpr std::uint8_t kVdp2LayerRbg0 = 6U;
constexpr std::uint8_t kVdp2PixelSecondValid = 0x01U;
constexpr std::uint8_t kVdp2PixelTopColorCalculation = 0x02U;
constexpr std::uint8_t kVdp2PixelThirdValid = 0x04U;
constexpr std::uint8_t kVdp2PixelTopScreenColorCalculation = 0x08U;
constexpr std::uint8_t kVdp2PixelSecondScreenColorCalculation = 0x10U;
constexpr std::size_t kVdp1TextureTraceBegin = 0x60000U;
constexpr std::size_t kVdp1TextureTraceEnd = 0x60A80U;
constexpr std::size_t kVdp1TextureTraceLimit = 4096U;
constexpr std::size_t kVdp1RegisterTraceLimit = 4096U;

[[nodiscard]] std::uint32_t widthMask(std::uint8_t width) noexcept {
    return width == 1U ? 0xFFU : width == 2U ? 0xFFFFU : 0xFFFFFFFFU;
}

[[nodiscard]] std::uint16_t readRegisterWord(const std::vector<std::uint8_t>& memory,
                                              std::uint32_t offset) noexcept {
    if (offset + 1U >= memory.size()) {
        return 0U;
    }
    return static_cast<std::uint16_t>((memory[offset] << 8U) | memory[offset + 1U]);
}

[[nodiscard]] std::uint16_t interpolateShade(const std::array<std::uint16_t, 4>& colors,
                                             double u, double v) noexcept {
    std::uint16_t result{};
    for (unsigned shift : {0U, 5U, 10U}) {
        const auto a = static_cast<double>((colors[0] >> shift) & 31U);
        const auto b = static_cast<double>((colors[1] >> shift) & 31U);
        const auto c = static_cast<double>((colors[2] >> shift) & 31U);
        const auto d = static_cast<double>((colors[3] >> shift) & 31U);
        const auto value = (a + (b - a) * u) * (1.0 - v) + (d + (c - d) * u) * v;
        result |= static_cast<std::uint16_t>(std::clamp(static_cast<int>(value), 0, 31) << shift);
    }
    return result;
}

} // namespace

std::optional<std::uint32_t> VideoRuntime::read(core::Address address,
                                                std::uint8_t width) const {
    if (width != 1U && width != 2U && width != 4U) {
        return 0U;
    }
    if (address >= kVdp1Base && address < kVdp1Base + 0x80000U) {
        return readBytes(vdp1Vram_, address - kVdp1Base, width);
    }
    if (address >= kVdp1Framebuffer && address < kVdp1Framebuffer + 0x40000U) {
        synchronizeVdp1();
        return readBytes(vdp1Framebuffers_[vdp1DrawBank_],
                         address - kVdp1Framebuffer, width);
    }
    if (address >= kVdp1Registers && address < kVdp1Registers + kRegisterSize) {
        const auto offset = static_cast<std::uint32_t>(address - kVdp1Registers);
        if (width == 1U) {
            const auto word = vdp1Register(offset & ~1U);
            return (address & 1U) == 0U ? (word >> 8U) : (word & 0xFFU);
        }
        if ((offset & 1U) != 0U) return 0U;
        const auto word = vdp1Register(offset);
        return width == 2U ? word : (static_cast<std::uint32_t>(word) << 16U) | word;
    }
    if (address >= kVdp2Base && address < kVdp2Base + 0x80000U) {
        return readBytes(vdp2Vram_, address - kVdp2Base, width);
    }
    if (address >= kVdp2Cram && address < kVdp2Cram + kVdp2CramSize) {
        return readBytes(vdp2Cram_, address - kVdp2Cram, width);
    }
    if (address >= kVdp2Registers && address < kVdp2Registers + kRegisterSize) {
        const auto offset = static_cast<std::uint32_t>(address - kVdp2Registers);
        if (width == 1U) {
            const auto word = vdp2Register(offset & ~1U);
            return (address & 1U) == 0U ? (word >> 8U) : (word & 0xFFU);
        }
        if ((offset & 1U) != 0U) return 0U;
        const auto word = vdp2Register(offset);
        return width == 2U ? word : (static_cast<std::uint32_t>(word) << 16U) | word;
    }
    return 0U;
}

bool VideoRuntime::write(core::Address address, std::uint8_t width,
                         std::uint32_t value) {
    if (width != 1U && width != 2U && width != 4U) {
        return true;
    }
    value &= widthMask(width);
    const auto recordWrite = [&](auto& regions, std::size_t offset, std::size_t size) {
        if (regions.empty() || probeCaptured_) return;
        for (auto index = offset / 32U;
             index <= std::min(offset + width - 1U, size - 1U) / 32U; ++index) {
            ++regions[index].writes;
            regions[index].lastFrame = frameCount_;
        }
    };
    if (address >= kVdp1Base && address < kVdp1Base + 0x80000U) {
        const auto offset = static_cast<std::size_t>(address - kVdp1Base);
        if (vdp1MemoryWriteTraceEnabled_ && offset < kVdp1TextureTraceEnd &&
            offset + width > kVdp1TextureTraceBegin) {
            if (vdp1MemoryWriteHistory_.size() == kVdp1TextureTraceLimit) {
                vdp1MemoryWriteHistory_.pop_front();
            }
            vdp1MemoryWriteHistory_.push_back({contextStep_, contextBlock_,
                contextCaller_, vdp1MemoryWriteRegistersAvailable_,
                vdp1MemoryWriteRegisters_,
                static_cast<std::uint32_t>(offset), width, value});
        }
        writeBytes(vdp1Vram_, offset, width, value);
        return true;
    }
    if (address >= kVdp1Framebuffer && address < kVdp1Framebuffer + 0x40000U) {
        synchronizeVdp1();
        writeBytes(vdp1Framebuffers_[vdp1DrawBank_], address - kVdp1Framebuffer,
                   width, value);
        return true;
    }
    if (address >= kVdp1Registers && address < kVdp1Registers + kRegisterSize) {
        const auto offset = static_cast<std::uint32_t>(address - kVdp1Registers);
        if (width == 1U) {
            auto word = vdp1Register(offset & ~1U);
            if ((address & 1U) == 0U) {
                word = static_cast<std::uint16_t>((word & 0x00FFU) | (value << 8U));
            } else {
                word = static_cast<std::uint16_t>((word & 0xFF00U) | value);
            }
            writeVdp1Register(offset & ~1U, word);
        } else if ((offset & 1U) == 0U) {
            writeVdp1Register(offset, width == 2U
                ? static_cast<std::uint16_t>(value)
                : static_cast<std::uint16_t>(value >> 16U));
        }
        return true;
    }
    if (address >= kVdp2Base && address < kVdp2Base + 0x80000U) {
        recordWrite(vramWrites_, address - kVdp2Base, kVdp2VramSize);
        writeBytes(vdp2Vram_, address - kVdp2Base, width, value);
        return true;
    }
    if (address >= kVdp2Cram && address < kVdp2Cram + kVdp2CramSize) {
        recordWrite(cramWrites_, address - kVdp2Cram, kVdp2CramSize);
        writeBytes(vdp2Cram_, address - kVdp2Cram, width, value);
        return true;
    }
    if (address >= kVdp2Registers && address < kVdp2Registers + kRegisterSize) {
        const auto offset = static_cast<std::uint32_t>(address - kVdp2Registers);
        if (width == 1U) {
            auto word = vdp2Register(offset & ~1U);
            if ((address & 1U) == 0U) {
                word = static_cast<std::uint16_t>((word & 0x00FFU) | (value << 8U));
            } else {
                word = static_cast<std::uint16_t>((word & 0xFF00U) | value);
            }
            writeVdp2Register(offset & ~1U, word);
        } else if ((offset & 1U) == 0U) {
            writeVdp2Register(offset, width == 2U
                ? static_cast<std::uint16_t>(value)
                : static_cast<std::uint16_t>(value >> 16U));
            if (width == 4U && offset + 2U < kRegisterSize)
                writeVdp2Register(offset + 2U, static_cast<std::uint16_t>(value));
        }
        return true;
    }
    return true;
}

void VideoRuntime::advanceFrame(bool startAutomaticDraw) {
    // PTMR=2 is armed by its register write and starts after a bank change.
    // The scheduler passes false while a manual frame spans several fields;
    // explicit diagnostic advances retain their automatic-draw default.
    if (startAutomaticDraw && (vdp1Register(0x04U) & 0x3U) == 2U) startVdp1Draw();

    ++frameCount_;
    lastFrame_.sequence = frameCount_;
    compositionDirty_ = true;
    if (!deferredComposition_) composeFrame();
    framePending_ = true;
}

void VideoRuntime::setDeferredComposition(bool enabled) {
    deferredComposition_ = enabled;
    if (!enabled && compositionDirty_) composeFrame();
}

void VideoRuntime::composeFrame() {
    lastFrame_.height = frameHeight();
    lastFrame_.pixels.assign(static_cast<std::size_t>(kFrameWidth) * frameHeight(), 0xFF000000U);
    // Overlay resolves every output pixel, including empty sprite pixels and
    // shadows, after inserting VDP1 into the VDP2 candidate stack.
    composeVdp2(lastFrame_.pixels, nullptr, false);
    synchronizeVdp1();
    composeVdp1Overlay(lastFrame_.pixels);
    compositionDirty_ = false;
    ++compositionCount_;
}

std::uint64_t VideoRuntime::frameCount() const noexcept { return frameCount_; }
bool VideoRuntime::framePending() const noexcept { return framePending_; }
const VideoFrame& VideoRuntime::lastFrame() const noexcept { return lastFrame_; }

void VideoRuntime::setDebugEventSink(std::function<void(std::string_view)> sink) {
    debugEventSink_ = std::move(sink);
}

VideoDebugSnapshot VideoRuntime::captureDebugSnapshot(bool includeMemory) const {
    synchronizeVdp1();
    VideoDebugSnapshot snapshot;
    snapshot.composed = lastFrame_;
    snapshot.framebufferBanks = vdp1Framebuffers_;
    if (includeMemory) {
        snapshot.vdp1Vram = vdp1Vram_;
        snapshot.vdp2Vram = vdp2Vram_;
        snapshot.cram = vdp2Cram_;
        snapshot.vdp1Registers = vdp1Registers_;
        snapshot.vdp2Registers = vdp2Registers_;
        // These readable status words live outside the backing register arrays.
        for (const auto offset : {0x10U, 0x12U, 0x14U, 0x16U})
            writeBytes(snapshot.vdp1Registers, offset, 2U, vdp1Register(offset));
        writeBytes(snapshot.vdp2Registers, 0x04U, 2U, vdp2Register(0x04U));
    }
    snapshot.vdp1RegisterWriteHistory.assign(vdp1RegisterWriteHistory_.begin(),
                                             vdp1RegisterWriteHistory_.end());
    snapshot.vdp1MemoryWriteHistory.assign(vdp1MemoryWriteHistory_.begin(),
                                           vdp1MemoryWriteHistory_.end());
    snapshot.vdp1CommandHistory = vdp1CommandHistory_;
    snapshot.contextStep = contextStep_;
    snapshot.contextBlock = contextBlock_;
    snapshot.contextCaller = contextCaller_;
    snapshot.frameCount = frameCount_;
    snapshot.compositionCount = compositionCount_;
    snapshot.vdp1Tvmr = vdp1Register(0x00U);
    snapshot.vdp1Fbcr = vdp1Register(0x02U);
    snapshot.vdp1Ptmr = vdp1Register(0x04U);
    snapshot.vdp1Ewdr = vdp1Register(0x06U);
    snapshot.vdp1Ewlr = vdp1Register(0x08U);
    snapshot.vdp1Ewrr = vdp1Register(0x0AU);
    snapshot.vdp1Edsr = vdp1Edsr_;
    snapshot.vdp1Lopr = vdp1Lopr_;
    snapshot.vdp1Copr = vdp1Copr_;
    snapshot.vdp1DisplayBank = vdp1DisplayBank_;
    snapshot.vdp1DrawBank = vdp1DrawBank_;
    snapshot.vdp1SwapPending = vdp1SwapPending_;
    snapshot.vdp1ErasePending = vdp1ErasePending_;
    snapshot.vdp1VblankErasePending = vdp1VblankErasePending_;
    snapshot.vdp1InVBlank = vdp1InVBlank_;
    snapshot.vdp1DrawEndPending = vdp1DrawEndPending_;
    snapshot.vdp1FramebufferConfigured = vdp1FramebufferConfigured_;
    snapshot.compositionDirty = compositionDirty_;
    snapshot.framePending = framePending_;
    return snapshot;
}

bool VideoRuntime::consumeFrame(VideoFrame& output) {
    if (!framePending_) return false;
    output = lastFrame_;
    framePending_ = false;
    return true;
}

void VideoRuntime::presentFrame() {
    static_cast<void>(changeVdp1Framebuffer(frameCount_));
    presentCurrentFrame();
}

void VideoRuntime::onVBlankIn() {
    vdp1InVBlank_ = true;
    vdp1VblankErasePending_ = (vdp1Register(0x00U) & 0x8U) != 0U;
}

void VideoRuntime::onVBlankOut() {
    const auto changed = changeVdp1Framebuffer(frameCount_ + 1U);
    vdp1InVBlank_ = false;
    advanceFrame(changed);
    presentCurrentFrame();
}

bool VideoRuntime::changeVdp1Framebuffer(std::uint64_t presentedFrame) {
    const bool automatic = vdp1FramebufferConfigured_ &&
        (vdp1Register(0x02U) & 0x2U) == 0U;
    // VBlank erase operates on the display side before it becomes the next
    // draw side. It bypasses command clipping and accepts an EWDR of zero.
    if (automatic || (vdp1Register(0x00U) & 0x8U) != 0U ||
        vdp1VblankErasePending_ || vdp1ErasePending_) {
        if (debugEventSink_) debugEventSink_("before-erase");
        eraseVdp1DisplayFramebuffer();
        vdp1ErasePending_ = false;
        compositionDirty_ = true;
        if (debugEventSink_) debugEventSink_("after-erase");
    }
    vdp1VblankErasePending_ = false;
    // In manual-change mode FBCR selects which bank is displayed and which
    // bank receives the next VDP1 command list. The hardware changes the
    // visible side at the presentation boundary, not when PTMR starts the
    // draw. Recompose after the swap so the host never sees the old side.
    if (automatic || vdp1SwapPending_) {
        const auto nextDisplayBank = vdp1DrawBank_;
        for (auto execution = vdp1CommandHistory_.rbegin();
             execution != vdp1CommandHistory_.rend(); ++execution) {
            if (!execution->wasPresented &&
                execution->drawBank == nextDisplayBank) {
                execution->presentedFrame = presentedFrame;
                execution->wasPresented = true;
                execution->displayedBank = nextDisplayBank;
                break;
            }
        }
        vdp1DisplayBank_ = vdp1DrawBank_;
        vdp1DrawBank_ ^= 1U;
        vdp1SwapPending_ = false;
        compositionDirty_ = true;
        framePending_ = true;
        if (debugEventSink_) debugEventSink_("after-swap");
        return true;
    }
    return false;
}

void VideoRuntime::presentCurrentFrame() {
    if (compositionDirty_) composeFrame();
    bool capture = diagnosticCaptureEnabled_ && static_cast<bool>(diagnosticSink_);
    if (blackProbe_) {
        const bool black = std::all_of(lastFrame_.pixels.begin(), lastFrame_.pixels.end(),
            [](auto pixel) { return (pixel & 0x00FFFFFFU) == 0U; });
        if (displayEnabled() && !black) probeSawVisible_ = true;
        if (probeSawVisible_ && !displayEnabled()) probeSawOff_ = true;
        capture = capture && !probeCaptured_ && probeSawOff_ && black &&
                  vdp2Register(0x000U) == 0x8010U;
    }
    if (capture) {
        VideoDiagnosticSnapshot snapshot;
        if (blackProbe_ || diagnosticMemoryCapture_) {
            snapshot.vramDump = vdp2Vram_;
            snapshot.cramDump = vdp2Cram_;
            snapshot.registerDump = vdp2Registers_;
            snapshot.vdp1VramDump = vdp1Vram_;
        }
        if (blackProbe_) {
            snapshot.vramWrites = vramWrites_;
            snapshot.cramWrites = cramWrites_;
        }
        snapshot.priorityConfigured = vdp2PriorityConfigured_;
        snapshot.composed = lastFrame_;
        const auto makeBlankFrame = [sequence = lastFrame_.sequence, height = frameHeight()] {
            return VideoFrame{
                kFrameWidth, height, sequence,
                std::vector<std::uint32_t>(static_cast<std::size_t>(kFrameWidth) * height,
                                           0xFF000000U)};
        };
        snapshot.vdp2 = makeBlankFrame();
        composeVdp2(snapshot.vdp2.pixels, &snapshot.vdp2Layers);
        snapshot.back = makeBlankFrame();
        composeVdp2Back(snapshot.back.pixels);
        const auto backUpper = vdp2Register(0x0ACU);
        const auto backLower = vdp2Register(0x0AEU);
        const auto vramSize = vdp2Register(0x006U);
        const auto backUpperMask = (vramSize & 0x8000U) != 0U ? 0x7U : 0x3U;
        snapshot.vdp2BackAddress = static_cast<std::uint32_t>(
            (((backUpper & backUpperMask) << 16U) | backLower) * 2U);
        snapshot.vdp2BackWord = vdp2VramWord(snapshot.vdp2BackAddress);
        snapshot.vdp2BackLineMode = (backUpper & 0x8000U) != 0U;
        snapshot.vdp2BackLineWords.assign(16U, 0U);
        for (std::size_t line = 0; line < snapshot.vdp2BackLineWords.size(); ++line) {
            const auto sourceLine = snapshot.vdp2BackLineMode ? line : 0U;
            snapshot.vdp2BackLineWords[line] = vdp2VramWord(
                snapshot.vdp2BackAddress + sourceLine * 2U);
        }

        snapshot.vdp2Bgon = vdp2Register(0x020U);
        snapshot.vdp2Chctla = vdp2Register(0x028U);
        snapshot.vdp2Bmpna = vdp2Register(0x02CU);
        snapshot.vdp2Mpofn = vdp2Register(0x03CU);
        const auto nbg0ScrollX = vdp2Register(0x070U);
        const auto nbg0ScrollY = vdp2Register(0x074U);
        snapshot.nbg0 = makeBlankFrame();
        if ((snapshot.vdp2Bgon & kVdp2Nbg0Enable) != 0U) {
            if ((snapshot.vdp2Chctla & 0x0002U) != 0U) {
                composeVdp2Bitmap(snapshot.nbg0.pixels, 0U, snapshot.vdp2Chctla,
                                  snapshot.vdp2Bmpna, snapshot.vdp2Mpofn,
                                  nbg0ScrollX, nbg0ScrollY);
            } else {
                composeVdp2Cell(snapshot.nbg0.pixels, 0U, snapshot.vdp2Chctla,
                                vdp2Register(0x030U), snapshot.vdp2Mpofn,
                                vdp2Register(0x040U), vdp2Register(0x042U),
                                nbg0ScrollX, nbg0ScrollY,
                                blackProbe_ ? &snapshot.cellSamples : nullptr);
            }
        }
        snapshot.vdp1Banks[0] = makeVdp1DiagnosticFrame(0U);
        snapshot.vdp1Banks[1] = makeVdp1DiagnosticFrame(1U);
        snapshot.vdp1Bounds[0] = pixelBounds(snapshot.vdp1Banks[0]);
        snapshot.vdp1Bounds[1] = pixelBounds(snapshot.vdp1Banks[1]);
        snapshot.vdp2Bounds = pixelBounds(snapshot.vdp2);
        for (std::size_t layer = 0; layer < snapshot.vdp2Layers.size(); ++layer) {
            snapshot.vdp2LayerBounds[layer] = pixelBounds(snapshot.vdp2Layers[layer]);
        }
        snapshot.backBounds = pixelBounds(snapshot.back);
        snapshot.nbg0Bounds = pixelBounds(snapshot.nbg0);
        snapshot.composedBounds = pixelBounds(snapshot.composed);
        snapshot.vdp1Memory = memoryBounds(vdp1Framebuffers_[vdp1DisplayBank_]);
        snapshot.vdp2VramMemory = memoryBounds(vdp2Vram_);
        snapshot.vdp2CramMemory = memoryBounds(vdp2Cram_);

        snapshot.vdp1Tvmr = vdp1Register(0x00U);
        snapshot.vdp1Fbcr = vdp1Register(0x02U);
        snapshot.vdp1Ptmr = vdp1Register(0x04U);
        snapshot.vdp1Ewdr = vdp1Register(0x06U);
        snapshot.vdp1Ewlr = vdp1Register(0x08U);
        snapshot.vdp1Ewrr = vdp1Register(0x0AU);
        snapshot.vdp1Edsr = vdp1Register(0x10U);
        snapshot.vdp1Lopr = vdp1Register(0x12U);
        snapshot.vdp1Copr = vdp1Register(0x14U);
        snapshot.vdp1DisplayBank = vdp1DisplayBank_;
        snapshot.vdp1DrawBank = vdp1DrawBank_;
        snapshot.vdp1SwapPending = vdp1SwapPending_;
        snapshot.vdp1CommandHistory = vdp1CommandHistory_;
        snapshot.vdp1MemoryWriteHistory.assign(vdp1MemoryWriteHistory_.begin(),
                                               vdp1MemoryWriteHistory_.end());

        snapshot.vdp2Tvmd = vdp2Register(0x000U);
        snapshot.vdp2Chctlb = vdp2Register(0x02AU);
        snapshot.vdp2Bmpnb = vdp2Register(0x02EU);
        snapshot.vdp2Mpofr = vdp2Register(0x03EU);
        snapshot.vdp2Prina = vdp2Register(0x0F8U);
        snapshot.vdp2Prinb = vdp2Register(0x0FAU);
        snapshot.vdp2Prir = vdp2Register(0x0FCU);
        snapshot.vdp2Spctl = vdp2Register(0x0E0U);
        snapshot.vdp2Ccctl = vdp2Register(0x0ECU);
        snapshot.vdp2Wctla = vdp2Register(0x0D0U);
        snapshot.vdp2Wpsx0 = vdp2Register(0x0C0U);
        snapshot.vdp2Wpsy0 = vdp2Register(0x0C2U);
        snapshot.vdp2Wpex0 = vdp2Register(0x0C4U);
        snapshot.vdp2Wpey0 = vdp2Register(0x0C6U);
        snapshot.vdp2Scrctl = vdp2Register(0x09AU);
        snapshot.vdp2Vcstau = vdp2Register(0x09CU);
        snapshot.vdp2Vcstal = vdp2Register(0x09EU);
        snapshot.vdp2Lsta0u = vdp2Register(0x0A0U);
        snapshot.vdp2Lsta0l = vdp2Register(0x0A2U);
        const auto tableAddress = [](std::uint16_t upper,
                                     std::uint16_t lower) {
            // The upper register supplies address bits 18..16 and the lower
            // register supplies bits 15..1.  VDP2 addresses are word based.
            return static_cast<std::size_t>(((upper & 0x0007U) << 16U) |
                                             (lower & 0xFFFEU)) * 2U;
        };
        const auto lineScrollAddress = tableAddress(snapshot.vdp2Lsta0u,
                                                     snapshot.vdp2Lsta0l);
        const auto verticalCellAddress = tableAddress(snapshot.vdp2Vcstau,
                                                       snapshot.vdp2Vcstal);
        // Keep enough words to cover the first displayed lines.  Each line
        // uses a 32-bit pair, so 128 halfwords cover 64 lines and expose the
        // first non-zero entry even when the bitmap is vertically centered.
        constexpr std::size_t kScrollSampleWords = 128U;
        snapshot.vdp2LineScrollWords.assign(kScrollSampleWords, 0U);
        snapshot.vdp2VerticalCellScrollWords.assign(kScrollSampleWords, 0U);
        for (std::size_t index = 0; index < kScrollSampleWords; ++index) {
            snapshot.vdp2LineScrollWords[index] = vdp2VramWord(
                lineScrollAddress + index * 2U);
            snapshot.vdp2VerticalCellScrollWords[index] = vdp2VramWord(
                verticalCellAddress + index * 2U);
        }
        constexpr std::size_t kScrollScanWords = 512U;
        for (std::size_t index = 0; index < kScrollScanWords; ++index) {
            const auto lineWord = vdp2VramWord(lineScrollAddress + index * 2U);
            if (lineWord != 0U) {
                if (snapshot.vdp2LineScrollNonZeroWords == 0U) {
                    snapshot.vdp2LineScrollFirstNonZero = index;
                }
                ++snapshot.vdp2LineScrollNonZeroWords;
            }
            const auto cellWord = vdp2VramWord(verticalCellAddress + index * 2U);
            if (cellWord != 0U) {
                if (snapshot.vdp2VerticalCellScrollNonZeroWords == 0U) {
                    snapshot.vdp2VerticalCellScrollFirstNonZero = index;
                }
                ++snapshot.vdp2VerticalCellScrollNonZeroWords;
            }
        }

        const auto bitmap = (snapshot.vdp2Chctla & 0x0002U) != 0U;
        const auto colors = static_cast<unsigned>((snapshot.vdp2Chctla >> 4U) & 0x7U);
        snapshot.vdp2Nbg0Base = static_cast<std::uint32_t>(
            (snapshot.vdp2Mpofn & 0x7U) * 0x20000U);
        snapshot.vdp2Nbg0Width = 320U;
        snapshot.vdp2Nbg0Height = 224U;
        if (bitmap) {
            const auto size = static_cast<unsigned>((snapshot.vdp2Chctla >> 2U) & 0x3U);
            snapshot.vdp2Nbg0Width = (size & 0x2U) != 0U ? 1024U : 512U;
            snapshot.vdp2Nbg0Height = (size & 0x1U) != 0U ? 512U : 256U;
        }
        snapshot.vdp2Nbg0BitsPerPixel = bitmap
            ? (colors == 0U ? 4U : colors == 1U ? 8U : colors == 2U ? 16U
                : colors == 3U ? 16U : colors == 4U ? 32U : 16U)
            : 16U;
        snapshot.vdp2Nbg0RowPitch =
            (snapshot.vdp2Nbg0Width * snapshot.vdp2Nbg0BitsPerPixel + 7U) / 8U;
        snapshot.vdp2Nbg0LineFirstRaw.assign(frameHeight(), 0U);
        snapshot.vdp2Nbg0LineRawHash.assign(frameHeight(), 2166136261U);
        snapshot.vdp2Nbg0LineRawNonZero.assign(frameHeight(), 0U);
        snapshot.vdp2Nbg0LineAlphaNonZero.assign(frameHeight(), 0U);
        snapshot.vdp2Nbg0LineNonZero.assign(frameHeight(), 0U);
        const bool continuousNbg0 = bitmap &&
            (snapshot.vdp2Scrctl & 0x0007U) != 0U;
        const auto readVdp2Long = [this](std::size_t address) {
            return (static_cast<std::uint32_t>(vdp2VramWord(address)) << 16U) |
                vdp2VramWord(address + 2U);
        };
        // Line/cell scroll table entries are relative fixed-point values.
        // SCXIN/SCYIN (plus their decimal registers) provide the base that
        // must remain in the coordinate calculation when either table is
        // enabled.  Dropping this base makes the physical row carry occur at
        // the wrong horizontal cells, producing the right-growing stripes
        // seen in the Rockman X4 FMV.
        const auto nbg0ScrollXFixed =
            (static_cast<std::uint32_t>(nbg0ScrollX & 0x07FFU) << 16U) |
            (static_cast<std::uint32_t>(vdp2Register(0x072U)) & 0xFF00U);
        const auto nbg0ScrollYFixed =
            (static_cast<std::uint32_t>(nbg0ScrollY & 0x07FFU) << 16U) |
            (static_cast<std::uint32_t>(vdp2Register(0x076U)) & 0xFF00U);
        const auto n0LineInterval = continuousNbg0
            ? (1U << ((snapshot.vdp2Scrctl >> 4U) & 0x3U)) : 1U;
        const bool lineHorizontalEnabled = continuousNbg0 &&
            (snapshot.vdp2Scrctl & 0x0002U) != 0U;
        const bool lineVerticalEnabled = continuousNbg0 &&
            (snapshot.vdp2Scrctl & 0x0004U) != 0U;
        const bool cellVerticalEnabled = continuousNbg0 &&
            (snapshot.vdp2Scrctl & 0x0001U) != 0U;
        // Rockman X4 selects 320x240 timing while its FMV bitmap is 224
        // lines tall. The cropped host frame therefore starts at the
        // hardware's eighth active line; line-scroll data is indexed in that
        // hardware coordinate space.
        const auto vdp2LineOrigin =
            (snapshot.vdp2Tvmd & 0x0030U) == 0x0010U ? 8U : 0U;
        // Some Saturn streaming producers reserve an 8-byte slot per cell
        // and place NBG0's 32-bit value in the second longword (the first is
        // the unused/interleaved NBG1 slot).  Detect that representation from
        // the table itself; ordinary NBG0-only tables remain 4 bytes/cell.
        std::size_t cellScrollStride = 4U;
        std::size_t cellScrollBias = 0U;
        if (cellVerticalEnabled) {
            std::size_t directNonZero{};
            std::size_t interleavedNonZero{};
            for (std::size_t cell = 1U; cell < 32U; ++cell) {
                if (readVdp2Long(verticalCellAddress + cell * 4U) != 0U) {
                    ++directNonZero;
                }
                if (readVdp2Long(verticalCellAddress + 4U + cell * 8U) != 0U) {
                    ++interleavedNonZero;
                }
            }
            // The interleaved slots alias every other direct-table probe, so
            // comparing the two populations (rather than requiring a 2x
            // margin) keeps short but valid tables detectable as well.
            if (interleavedNonZero >= 4U &&
                interleavedNonZero > directNonZero) {
                cellScrollStride = 8U;
                cellScrollBias = 4U;
            }
        }
        const auto readVerticalCell = [&, cellScrollStride, cellScrollBias](
                                           std::size_t cell) {
            return readVdp2Long(verticalCellAddress + cellScrollBias +
                                cell * cellScrollStride);
        };
        for (std::uint32_t y = 0; y < frameHeight(); ++y) {
            auto rowHash = 2166136261U;
            std::size_t lineHorizontal = nbg0ScrollXFixed >> 16U;
            std::uint32_t lineVertical = nbg0ScrollYFixed;
            if (continuousNbg0) {
                const auto lineIndex = (static_cast<std::size_t>(y) +
                                        vdp2LineOrigin) / n0LineInterval;
                const auto lineAddress = lineScrollAddress + lineIndex * 8U;
                if (lineHorizontalEnabled) {
                    lineHorizontal = ((nbg0ScrollXFixed +
                                       readVdp2Long(lineAddress)) >> 16U) & 0x01FFU;
                }
                if (lineVerticalEnabled) {
                    lineVertical = nbg0ScrollYFixed + readVdp2Long(lineAddress + 4U);
                }
            }
            const auto firstVertical = cellVerticalEnabled
                ? lineVertical + readVerticalCell(0U) : lineVertical;
            const auto sourceY = continuousNbg0
                ? (static_cast<std::size_t>(firstVertical >> 16U) %
                   snapshot.vdp2Nbg0Height)
                : (static_cast<std::size_t>(y) + (nbg0ScrollY & 0x07FFU)) %
                  snapshot.vdp2Nbg0Height;
            const auto sourceX = lineHorizontal % snapshot.vdp2Nbg0Width;
            const auto offset = static_cast<std::size_t>(snapshot.vdp2Nbg0Base) +
                sourceY * snapshot.vdp2Nbg0RowPitch +
                sourceX * snapshot.vdp2Nbg0BitsPerPixel / 8U;
            if (snapshot.vdp2Nbg0BitsPerPixel == 32U) {
                snapshot.vdp2Nbg0LineFirstRaw[y] =
                    (static_cast<std::uint32_t>(vdp2VramWord(offset)) << 16U) |
                    vdp2VramWord(offset + 2U);
            } else {
                snapshot.vdp2Nbg0LineFirstRaw[y] = vdp2VramWord(offset);
            }
            const auto visibleWidth = std::min<std::uint32_t>(
                kFrameWidth, snapshot.vdp2Nbg0Width);
            for (std::uint32_t x = 0; x < visibleWidth; ++x) {
                const auto sourcePixelX = (static_cast<std::size_t>(x) +
                                           lineHorizontal) %
                                          snapshot.vdp2Nbg0Width;
                const auto vertical = cellVerticalEnabled
                    ? lineVertical + readVerticalCell(static_cast<std::size_t>(x) / 8U)
                    : lineVertical;
                const auto pixelSourceY = continuousNbg0
                    ? (static_cast<std::size_t>(vertical >> 16U) %
                       snapshot.vdp2Nbg0Height)
                    : sourceY;
                const auto pixelOffset = static_cast<std::size_t>(snapshot.vdp2Nbg0Base) +
                    pixelSourceY * snapshot.vdp2Nbg0RowPitch +
                    sourcePixelX * snapshot.vdp2Nbg0BitsPerPixel / 8U;
                if (snapshot.vdp2Nbg0BitsPerPixel == 32U) {
                    const auto raw = (static_cast<std::uint32_t>(
                        vdp2VramWord(pixelOffset)) << 16U) |
                        vdp2VramWord(pixelOffset + 2U);
                    rowHash ^= raw;
                    rowHash *= 16777619U;
                    if ((raw & 0x00FFFFFFU) != 0U) {
                        ++snapshot.vdp2Nbg0LineRawNonZero[y];
                    }
                    if ((raw & 0x80000000U) != 0U) {
                        ++snapshot.vdp2Nbg0LineAlphaNonZero[y];
                    }
                } else if (vdp2VramWord(pixelOffset) != 0U) {
                    const auto raw = static_cast<std::uint32_t>(vdp2VramWord(pixelOffset));
                    rowHash ^= raw;
                    rowHash *= 16777619U;
                    ++snapshot.vdp2Nbg0LineRawNonZero[y];
                } else {
                    rowHash ^= static_cast<std::uint32_t>(vdp2VramWord(pixelOffset));
                    rowHash *= 16777619U;
                }
            }
            snapshot.vdp2Nbg0LineRawHash[y] = rowHash;
            const auto rowStart = static_cast<std::size_t>(y) * kFrameWidth;
            snapshot.vdp2Nbg0LineNonZero[y] = static_cast<std::size_t>(std::count_if(
                snapshot.nbg0.pixels.begin() + rowStart,
                snapshot.nbg0.pixels.begin() + rowStart + kFrameWidth,
                [](std::uint32_t pixel) { return pixel != 0xFF000000U; }));
        }
        diagnosticSink_(snapshot);
        if (blackProbe_) probeCaptured_ = true;
    }
    if (frameSink_) frameSink_(lastFrame_);
}

void VideoRuntime::setFrameSink(VideoFrameSink sink) {
    frameSink_ = std::move(sink);
}

void VideoRuntime::setDiagnosticSink(VideoDiagnosticSink sink) {
    diagnosticSink_ = std::move(sink);
}

void VideoRuntime::setDiagnosticCaptureEnabled(bool enabled) noexcept {
    diagnosticCaptureEnabled_ = enabled;
}

void VideoRuntime::setDiagnosticMemoryCapture(bool enabled) noexcept {
    diagnosticMemoryCapture_ = enabled;
}

void VideoRuntime::setVdp1CommandTraceEnabled(bool enabled) noexcept {
    vdp1CommandTraceEnabled_ = enabled;
    if (!enabled) vdp1CommandHistory_.clear();
}

void VideoRuntime::setVdp1MemoryWriteTraceEnabled(bool enabled) noexcept {
    vdp1MemoryWriteTraceEnabled_ = enabled;
    if (!enabled) vdp1MemoryWriteHistory_.clear();
    if (!enabled && !debugEventSink_) vdp1RegisterWriteHistory_.clear();
}

bool VideoRuntime::vdp1MemoryWriteTraceEnabled() const noexcept {
    return vdp1MemoryWriteTraceEnabled_;
}

void VideoRuntime::setVdp1MemoryWriteRegisters(
    const std::array<std::uint32_t, 8>& registers, bool available) noexcept {
    vdp1MemoryWriteRegisters_ = registers;
    vdp1MemoryWriteRegistersAvailable_ = available;
}

void VideoRuntime::setExecutionContext(std::size_t step, core::Address block,
                                       core::Address caller) noexcept {
    contextStep_ = step;
    contextBlock_ = block;
    contextCaller_ = caller;
}

void VideoRuntime::setMapReadTrace(bool enabled) {
    mapReadTraceEnabled_ = enabled;
    if (enabled) {
        mapReadTrace_.clear();
        mapStateTrace_.clear();
    }
}

const std::vector<VideoMapReadRecord>& VideoRuntime::mapReadTrace() const noexcept {
    return mapReadTrace_;
}

const std::vector<VideoMapStateRecord>& VideoRuntime::mapStateTrace() const noexcept {
    return mapStateTrace_;
}

void VideoRuntime::captureFirstReenabledBlackFrame(VideoDiagnosticSink sink) {
    diagnosticSink_ = std::move(sink);
    blackProbe_ = true;
    probeSawVisible_ = probeSawOff_ = probeCaptured_ = false;
    vramWrites_.assign(kVdp2VramSize / 32U, {});
    cramWrites_.assign(kVdp2CramSize / 32U, {});
}

bool VideoRuntime::displayEnabled() const noexcept {
    return (vdp2Register(0x000U) & 0x8000U) != 0U;
}

const Vdp1CommandStats& VideoRuntime::vdp1CommandStats() const noexcept {
    return vdp1CommandStats_;
}

bool VideoRuntime::takeVdp1DrawEnd() noexcept {
    const auto pending = vdp1DrawEndPending_;
    vdp1DrawEndPending_ = false;
    return pending;
}

std::uint32_t VideoRuntime::readBytes(const std::vector<std::uint8_t>& memory,
                                      std::size_t offset, std::uint8_t width) noexcept {
    if (offset >= memory.size() || offset + width > memory.size()) return 0U;
    std::uint32_t value{};
    for (std::uint8_t index = 0; index < width; ++index) {
        value = (value << 8U) | memory[offset + index];
    }
    return value;
}

void VideoRuntime::writeBytes(std::vector<std::uint8_t>& memory, std::size_t offset,
                              std::uint8_t width, std::uint32_t value) noexcept {
    if (offset >= memory.size() || offset + width > memory.size()) return;
    for (std::uint8_t index = 0; index < width; ++index) {
        const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
        memory[offset + index] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::uint16_t VideoRuntime::vdp1Register(std::uint32_t offset) const noexcept {
    switch (offset) {
    case 0x10U: return vdp1Edsr_;
    case 0x12U: return vdp1Lopr_;
    case 0x14U: return vdp1Copr_;
    case 0x16U:
        return static_cast<std::uint16_t>(0x1000U | ((vdp1Register(0x04U) & 2U) << 7U) |
            ((vdp1Register(0x02U) & 0x1EU) << 3U) | (vdp1Register(0x00U) & 0xFU));
    default: return readRegisterWord(vdp1Registers_, offset);
    }
}

std::uint16_t VideoRuntime::vdp2Register(std::uint32_t offset) const noexcept {
    if (offset == 0x004U) {
        return static_cast<std::uint16_t>(vdp2Tvstat_ | (displayEnabled() ? 0U : 0x0008U));
    }
    return readRegisterWord(vdp2Registers_, offset);
}

std::uint32_t VideoRuntime::frameHeight() const noexcept {
    // Normal non-interlaced TVMD modes used by this runtime. The VDP1
    // 16-bit framebuffer has 256 rows; output may select 224, 240 or 256.
    const auto vertical = (vdp2Register(0x000U) >> 4U) & 0x3U;
    return vertical == 1U ? 240U : vertical == 2U ? 256U : kFrameHeight;
}

void VideoRuntime::writeVdp1Register(std::uint32_t offset, std::uint16_t value) {
    if (offset == 0x10U || offset == 0x12U || offset == 0x14U || offset == 0x16U) return;
    if (vdp1MemoryWriteTraceEnabled_ || debugEventSink_) {
        if (vdp1RegisterWriteHistory_.size() == kVdp1RegisterTraceLimit)
            vdp1RegisterWriteHistory_.pop_front();
        vdp1RegisterWriteHistory_.push_back({contextStep_, contextBlock_,
            contextCaller_, vdp1MemoryWriteRegistersAvailable_,
            vdp1MemoryWriteRegisters_, 0x100000U + offset, 2U, value});
    }
    if (vdp1MemoryWriteTraceEnabled_) {
        if (vdp1MemoryWriteHistory_.size() == kVdp1TextureTraceLimit)
            vdp1MemoryWriteHistory_.pop_front();
        vdp1MemoryWriteHistory_.push_back({contextStep_, contextBlock_,
            contextCaller_, vdp1MemoryWriteRegistersAvailable_,
            vdp1MemoryWriteRegisters_, 0x100000U + offset, 2U, value});
    }
    writeBytes(vdp1Registers_, offset, 2U, value);
    if (offset == 0x00U && vdp1InVBlank_ && (value & 0x8U) != 0U)
        vdp1VblankErasePending_ = true;
    if (offset == 0x02U) {
        if (!vdp1FramebufferConfigured_) {
            vdp1DrawBank_ = vdp1DisplayBank_ ^ 1U;
            vdp1FramebufferConfigured_ = true;
        }
        // FCT is a one-field request, including repeated writes of 3.
        // Drawing alone must never request a manual bank change.
        vdp1ErasePending_ = (value & 0x3U) == 0x2U;
        vdp1SwapPending_ = (value & 0x3U) == 0x3U;
    }
    if (offset == 0x04U && (value & 0x3U) == 1U) {
        startVdp1Draw();
        advanceFrame();
    }
}

void VideoRuntime::writeVdp2Register(std::uint32_t offset, std::uint16_t value) {
    if (offset == 0x004U || offset == 0x008U || offset == 0x00AU) return;
    writeBytes(vdp2Registers_, offset, 2U, value);
    if (offset == 0x0F8U || offset == 0x0FAU || offset == 0x0FCU) {
        vdp2PriorityConfigured_ = true;
    }
}

void VideoRuntime::startVdp1Draw() {
    if (debugEventSink_) debugEventSink_("before-draw");
    vdp1Copr_ = 0U;
    executeVdp1Commands();
    // The VDP1 command-end factor is sampled by the host scheduler after the
    // current block, matching the non-preemptive event boundary used by DMA.
    vdp1DrawEndPending_ = true;
    if (debugEventSink_) debugEventSink_("after-draw");
}

void VideoRuntime::eraseVdp1DisplayFramebuffer() {
    if (vdp1GpuPending_ && vdp1GpuBank_ == vdp1DisplayBank_) synchronizeVdp1();
    const auto color = vdp1Register(0x06U);
    const auto left = vdp1Register(0x08U);
    const auto right = vdp1Register(0x0AU);
    const auto x0 = static_cast<unsigned>((left >> 9U) & 0x3FU) * 8U;
    const auto y0 = static_cast<unsigned>(left & 0x1FFU);
    const auto x1 = std::min(static_cast<unsigned>((right >> 9U) & 0x7FU) * 8U + 8U,
                             kVdp1FramebufferWidth);
    const auto y1 = std::min(static_cast<unsigned>(right & 0x1FFU) + 1U, 256U);
    auto& framebuffer = vdp1Framebuffers_[vdp1DisplayBank_];
    for (auto y = y0; y < y1; ++y) {
        for (auto x = x0; x < x1; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * kVdp1FramebufferWidth + x) * 2U;
            framebuffer[offset] = static_cast<std::uint8_t>(color >> 8U);
            framebuffer[offset + 1U] = static_cast<std::uint8_t>(color);
        }
    }
}

std::uint16_t VideoRuntime::vramWord(std::size_t offset) const noexcept {
    return static_cast<std::uint16_t>(readBytes(vdp1Vram_, offset, 2U));
}

std::uint16_t VideoRuntime::vdp2VramWord(std::size_t offset) const noexcept {
    offset &= kVdp2VramSize - 1U;
    return static_cast<std::uint16_t>(readBytes(vdp2Vram_, offset, 2U));
}

std::uint16_t VideoRuntime::vdp2CramWord(std::size_t offset) const noexcept {
    offset &= kVdp2CramSize - 2U;
    return static_cast<std::uint16_t>(readBytes(vdp2Cram_, offset, 2U));
}

VideoFrame VideoRuntime::makeVdp1DiagnosticFrame(unsigned bank) const {
    synchronizeVdp1();
    VideoFrame frame{
        kFrameWidth, frameHeight(), frameCount_,
        std::vector<std::uint32_t>(static_cast<std::size_t>(kFrameWidth) * frameHeight(),
                                   0xFF000000U)};
    if (bank >= vdp1Framebuffers_.size()) return frame;
    const auto& framebuffer = vdp1Framebuffers_[bank];
    for (std::uint32_t y = 0; y < frameHeight(); ++y) {
        for (std::uint32_t x = 0; x < kFrameWidth; ++x) {
            const auto index = static_cast<std::size_t>(y) * kVdp1FramebufferWidth + x;
            const auto value = static_cast<std::uint16_t>(
                (framebuffer[index * 2U] << 8U) | framebuffer[index * 2U + 1U]);
            if (value == 0U) continue;
            std::uint8_t priority{};
            std::uint8_t colorCalculationRatioIndex{};
            const auto color = decodeVdp1FramebufferPixel(
                value, priority, colorCalculationRatioIndex);
            if (color != 0U) {
                frame.pixels[static_cast<std::size_t>(y) * frame.width + x] = color;
            }
        }
    }
    return frame;
}

VideoPixelBounds VideoRuntime::pixelBounds(const VideoFrame& frame) noexcept {
    VideoPixelBounds bounds{};
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const auto pixel = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
            if (pixel == 0xFF000000U) continue;
            if (bounds.nonZeroPixels == 0U) {
                bounds.minX = bounds.maxX = x;
                bounds.minY = bounds.maxY = y;
            } else {
                bounds.minX = std::min(bounds.minX, x);
                bounds.minY = std::min(bounds.minY, y);
                bounds.maxX = std::max(bounds.maxX, x);
                bounds.maxY = std::max(bounds.maxY, y);
            }
            ++bounds.nonZeroPixels;
        }
    }
    return bounds;
}

VideoMemoryBounds VideoRuntime::memoryBounds(
    const std::vector<std::uint8_t>& memory) noexcept {
    VideoMemoryBounds bounds{};
    for (std::size_t offset = 0; offset < memory.size(); ++offset) {
        if (memory[offset] == 0U) continue;
        if (bounds.nonZeroBytes == 0U) bounds.firstByte = offset;
        bounds.lastByte = offset;
        ++bounds.nonZeroBytes;
    }
    return bounds;
}

namespace {

[[nodiscard]] std::uint32_t rgb555ToArgb(std::uint16_t value) noexcept {
    const auto red = static_cast<std::uint32_t>((value & 0x1FU) * 255U / 31U);
    const auto green = static_cast<std::uint32_t>(((value >> 5U) & 0x1FU) * 255U / 31U);
    const auto blue = static_cast<std::uint32_t>(((value >> 10U) & 0x1FU) * 255U / 31U);
    return 0xFF000000U | (red << 16U) | (green << 8U) | blue;
}

[[nodiscard]] std::uint32_t rgb888ToArgb(std::uint32_t value) noexcept {
    // VDP2 stores 24-bit scroll RGB as B:G:R in bits 23..0. The upper byte
    // of the 32-bit VRAM fetch is not an alpha bit and must not discard dots.
    const auto red = value & 0xFFU;
    const auto green = (value >> 8U) & 0xFFU;
    const auto blue = (value >> 16U) & 0xFFU;
    return 0xFF000000U | (red << 16U) | (green << 8U) | blue;
}

[[nodiscard]] std::uint32_t addVdp2Transparency(std::uint16_t value) noexcept {
    return value == 0U ? 0U : rgb555ToArgb(value);
}

[[nodiscard]] bool vdp2SpecialColorCodeMatches(std::uint16_t select,
                                                std::uint16_t codes,
                                                unsigned layer,
                                                unsigned dotColorCode) noexcept {
    const auto codeBank = static_cast<unsigned>((select >> layer) & 0x1U);
    const auto codeTable = static_cast<unsigned>((codes >> (codeBank * 8U)) & 0xFFU);
    const auto codeBit = (dotColorCode & 0x0FU) >> 1U;
    return (codeTable & (1U << codeBit)) != 0U;
}

[[nodiscard]] unsigned vdp2LayerTieRank(std::uint8_t layer) noexcept {
    switch (layer) {
    case kVdp2LayerSprite: return 7U;
    case kVdp2LayerRbg0: return 6U;
    case kVdp2LayerNbg0: return 5U;
    case kVdp2LayerNbg1: return 4U;
    case kVdp2LayerNbg2: return 3U;
    case kVdp2LayerNbg3: return 2U;
    case kVdp2LayerBack: return 1U;
    default: return 0U;
    }
}

[[nodiscard]] bool isAboveVdp2Candidate(std::uint8_t priority,
                                         std::uint8_t layer,
                                         std::uint8_t otherPriority,
                                         std::uint8_t otherLayer) noexcept {
    return priority > otherPriority ||
        (priority == otherPriority && vdp2LayerTieRank(layer) >
                                      vdp2LayerTieRank(otherLayer));
}

[[nodiscard]] std::uint8_t colorComponent(std::uint32_t color,
                                           unsigned shift) noexcept {
    return static_cast<std::uint8_t>((color >> shift) & 0xFFU);
}

[[nodiscard]] std::uint32_t calculateVdp2Color(std::uint32_t top,
                                                std::uint32_t second,
                                                std::uint8_t ratio,
                                                bool addAsIs) noexcept {
    const auto calculate = [ratio, addAsIs](std::uint8_t foreground,
                                            std::uint8_t background) {
        if (addAsIs) {
            return static_cast<std::uint8_t>(std::min<unsigned>(
                255U, static_cast<unsigned>(foreground) + background));
        }
        const auto foregroundWeight = 31U - ratio;
        const auto backgroundWeight = 1U + ratio;
        return static_cast<std::uint8_t>(
            (static_cast<unsigned>(foreground) * foregroundWeight +
             static_cast<unsigned>(background) * backgroundWeight) >> 5U);
    };
    return 0xFF000000U |
        (static_cast<std::uint32_t>(calculate(colorComponent(top, 16U),
                                               colorComponent(second, 16U))) << 16U) |
        (static_cast<std::uint32_t>(calculate(colorComponent(top, 8U),
                                               colorComponent(second, 8U))) << 8U) |
        calculate(colorComponent(top, 0U), colorComponent(second, 0U));
}

[[nodiscard]] std::uint32_t halveVdp2Color(std::uint32_t color) noexcept {
    return 0xFF000000U |
        (static_cast<std::uint32_t>(colorComponent(color, 16U) >> 1U) << 16U) |
        (static_cast<std::uint32_t>(colorComponent(color, 8U) >> 1U) << 8U) |
        (colorComponent(color, 0U) >> 1U);
}

[[nodiscard]] bool isNormalShadowColorCode(std::uint16_t value,
                                            unsigned spriteType) noexcept {
    std::uint16_t colorMask{};
    switch (spriteType) {
    case 0U: case 1U: case 2U: case 3U: case 5U:
        colorMask = 0x07FFU;
        break;
    case 4U: case 6U:
        colorMask = 0x03FFU;
        break;
    case 7U:
        colorMask = 0x01FFU;
        break;
    default:
        return false;
    }
    const auto shadowCode = static_cast<std::uint16_t>(colorMask & ~1U);
    return (value & colorMask) == shadowCode;
}

} // namespace

void VideoRuntime::composeVdp1Overlay(std::vector<std::uint32_t>& pixels) {
    const auto& framebuffer = vdp1Framebuffers_[vdp1DisplayBank_];
    const auto spriteControl = vdp2Register(0x0E0U);
    const auto spriteType = static_cast<unsigned>(spriteControl & 0x000FU);
    const auto mixedColorMode = (spriteControl & 0x0020U) != 0U;
    const auto shadowControl = vdp2Register(0x0E2U);
    const auto colorControl = vdp2Register(0x0ECU);
    const auto offsetEnable = vdp2Register(0x110U);
    const auto offsetSelect = vdp2Register(0x112U);
    std::array<std::array<int, 3>, 2> colorOffsets{};
    for (unsigned bank = 0U; bank < colorOffsets.size(); ++bank) {
        for (unsigned channel = 0U; channel < 3U; ++channel) {
            const auto value = static_cast<int>(
                vdp2Register(0x114U + bank * 6U + channel * 2U) & 0x1FFU);
            colorOffsets[bank][channel] = value >= 0x100 ? value - 0x200 : value;
        }
    }
    for (std::uint32_t y = 0; y < frameHeight(); ++y) {
        for (std::uint32_t x = 0; x < kFrameWidth; ++x) {
            const auto sourceIndex = static_cast<std::size_t>(y) * kVdp1FramebufferWidth + x;
            const auto outputIndex = static_cast<std::size_t>(y) * kFrameWidth + x;
            auto& composite = vdp2CompositePixels_[outputIndex];
            const auto value = static_cast<std::uint16_t>(
                (framebuffer[sourceIndex * 2U] << 8U) | framebuffer[sourceIndex * 2U + 1U]);
            bool shadowed{};
            if (value != 0U) {
                std::uint8_t spritePriority{};
                std::uint8_t ratioIndex{};
                const auto color = decodeVdp1FramebufferPixel(value, spritePriority,
                                                              ratioIndex);
                const auto paletteFormat = !mixedColorMode || (value & 0x8000U) == 0U;
                const auto normalShadow = paletteFormat &&
                    isNormalShadowColorCode(value, spriteType);
                const auto msbShadow = !mixedColorMode && spriteType >= 2U &&
                    spriteType <= 7U && (value & 0x8000U) != 0U;
                const auto transparentShadow = msbShadow &&
                    (value & 0x7FFFU) == 0U;

                if (spritePriority != 0U && (normalShadow || msbShadow)) {
                    const auto wins = isAboveVdp2Candidate(
                        spritePriority, kVdp2LayerSprite,
                        composite.topPriority, composite.topLayer);
                    const auto targetLayer = composite.topLayer;
                    const auto targetPriority = composite.topPriority;
                    const auto targetShadowBit = targetLayer == kVdp2LayerBack ? 5U :
                        targetLayer == kVdp2LayerRbg0 ? 4U : targetLayer;
                    const auto shadowLayerEnabled =
                        (targetLayer <= kVdp2LayerBack || targetLayer == kVdp2LayerRbg0) &&
                        (shadowControl & (1U << targetShadowBit)) != 0U;
                    const auto transparentShadowEnabled = !transparentShadow ||
                        (((shadowControl & 0x0100U) != 0U) &&
                         targetPriority + 1U == spritePriority);
                    if (wins && shadowLayerEnabled && transparentShadowEnabled) {
                        pixels[outputIndex] = halveVdp2Color(
                            resolveColorCalculation(composite));
                        shadowed = true;
                    }
                } else if (color != 0U && spritePriority != 0U) {
                    const auto colorCalculationRatio = static_cast<std::uint8_t>(
                        (vdp2Register(0x100U + (ratioIndex / 2U) * 2U) >>
                         ((ratioIndex & 1U) != 0U ? 8U : 0U)) & 0x1FU);
                    const auto condition = static_cast<unsigned>((spriteControl >> 12U) & 0x3U);
                    const auto conditionPriority = static_cast<unsigned>(
                        (spriteControl >> 8U) & 0x7U);
                    const auto spriteCalculationCondition =
                        condition == 0U ? spritePriority <= conditionPriority :
                        condition == 1U ? spritePriority == conditionPriority :
                        condition == 2U ? spritePriority >= conditionPriority :
                                          (value & 0x8000U) != 0U;
                    const auto spriteColorCalculation = (colorControl & 0x0040U) != 0U &&
                        spriteCalculationCondition;
                    const auto isTop = isAboveVdp2Candidate(
                        spritePriority, kVdp2LayerSprite,
                        composite.topPriority, composite.topLayer);
                    if (isTop) {
                        const auto oldTopScreenColorCalculation =
                            (composite.flags & kVdp2PixelTopScreenColorCalculation) != 0U;
                        if ((composite.flags & kVdp2PixelSecondValid) != 0U) {
                            composite.thirdColor = composite.secondColor;
                            composite.flags |= kVdp2PixelThirdValid;
                        } else {
                            composite.flags &= static_cast<std::uint8_t>(
                                ~kVdp2PixelThirdValid);
                        }
                        composite.secondColor = composite.topColor;
                        composite.secondPriority = composite.topPriority;
                        composite.secondLayer = composite.topLayer;
                        composite.secondRatio = composite.topRatio;
                        composite.topColor = color;
                        composite.topPriority = spritePriority;
                        composite.topLayer = kVdp2LayerSprite;
                        composite.topRatio = colorCalculationRatio;
                        composite.flags = static_cast<std::uint8_t>(
                            (composite.flags | kVdp2PixelSecondValid) &
                            ~(kVdp2PixelTopColorCalculation |
                              kVdp2PixelTopScreenColorCalculation |
                              kVdp2PixelSecondScreenColorCalculation));
                        if (oldTopScreenColorCalculation) {
                            composite.flags |= kVdp2PixelSecondScreenColorCalculation;
                        }
                        if (spriteColorCalculation) {
                            composite.flags |= kVdp2PixelTopColorCalculation;
                            composite.flags |= kVdp2PixelTopScreenColorCalculation;
                        }
                    } else if ((composite.flags & kVdp2PixelSecondValid) == 0U ||
                               isAboveVdp2Candidate(
                                   spritePriority, kVdp2LayerSprite,
                                   composite.secondPriority,
                                   composite.secondLayer)) {
                        if ((composite.flags & kVdp2PixelSecondValid) != 0U) {
                            composite.thirdColor = composite.secondColor;
                            composite.flags |= kVdp2PixelThirdValid;
                        } else {
                            composite.flags &= static_cast<std::uint8_t>(
                                ~kVdp2PixelThirdValid);
                        }
                        composite.secondColor = color;
                        composite.secondPriority = spritePriority;
                        composite.secondLayer = kVdp2LayerSprite;
                        composite.secondRatio = colorCalculationRatio;
                        composite.flags |= kVdp2PixelSecondValid;
                        composite.flags &= static_cast<std::uint8_t>(
                            ~kVdp2PixelSecondScreenColorCalculation);
                        if (spriteColorCalculation) {
                            composite.flags |= kVdp2PixelSecondScreenColorCalculation;
                        }
                    }
                }
            }
            if (!shadowed) pixels[outputIndex] = resolveColorCalculation(composite);
            // Color offset follows color calculation and uses the top image's
            // enable/bank selection, including when a sprite wins priority.
            const auto offsetBit = composite.topLayer == kVdp2LayerBack ? 5U :
                composite.topLayer == kVdp2LayerSprite ? 6U :
                composite.topLayer == kVdp2LayerRbg0 ? 4U : composite.topLayer;
            if ((offsetEnable & (1U << offsetBit)) != 0U) {
                const auto& offset = colorOffsets[(offsetSelect >> offsetBit) & 1U];
                const auto color = pixels[outputIndex];
                const auto red = std::clamp(static_cast<int>((color >> 16U) & 0xFFU) + offset[0], 0, 255);
                const auto green = std::clamp(static_cast<int>((color >> 8U) & 0xFFU) + offset[1], 0, 255);
                const auto blue = std::clamp(static_cast<int>(color & 0xFFU) + offset[2], 0, 255);
                pixels[outputIndex] = 0xFF000000U |
                    (static_cast<std::uint32_t>(red) << 16U) |
                    (static_cast<std::uint32_t>(green) << 8U) |
                    static_cast<std::uint32_t>(blue);
            }
        }
    }
}

std::uint32_t VideoRuntime::decodeVdp1FramebufferPixel(
    std::uint16_t value, std::uint8_t& priority,
    std::uint8_t& colorCalculationRatioIndex) const noexcept {
    priority = 0U;
    colorCalculationRatioIndex = 0U;
    if (value == 0U) return 0U;

    const auto spriteControl = vdp2Register(0x0E0U);
    const auto spriteType = static_cast<unsigned>(spriteControl & 0x000FU);
    const auto mixedColorMode = (spriteControl & 0x0020U) != 0U;
    const auto priorityForIndex = [this](unsigned index) {
        const auto registerOffset = 0x0F0U + (index / 2U) * 2U;
        const auto shift = (index & 1U) == 0U ? 0U : 8U;
        return static_cast<std::uint8_t>((vdp2Register(registerOffset) >> shift) & 0x7U);
    };

    // In mixed sprite mode an MSB of one selects RGB555. RGB pixels use
    // sprite register zero; palette pixels are decoded from the format
    // selected by SPTYPE below.
    if (mixedColorMode && (value & 0x8000U) != 0U) {
        priority = priorityForIndex(0U);
        return rgb555ToArgb(value);
    }

    unsigned priorityIndex{};
    std::uint16_t colorMask = 0x07FFU;
    switch (spriteType) {
    case 0U:
        priorityIndex = (value >> 14U) & 0x3U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 11U) & 0x7U);
        break;
    case 1U:
        priorityIndex = (value >> 13U) & 0x7U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 11U) & 0x3U);
        break;
    case 2U:
        priorityIndex = (value >> 14U) & 0x1U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 11U) & 0x7U);
        break;
    case 3U:
        priorityIndex = (value >> 13U) & 0x3U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 11U) & 0x3U);
        break;
    case 4U:
        priorityIndex = (value >> 13U) & 0x3U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 10U) & 0x7U);
        colorMask = 0x03FFU;
        break;
    case 5U:
        priorityIndex = (value >> 12U) & 0x7U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 11U) & 0x1U);
        break;
    case 6U:
        priorityIndex = (value >> 12U) & 0x7U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 10U) & 0x3U);
        colorMask = 0x03FFU;
        break;
    case 7U:
        priorityIndex = (value >> 12U) & 0x7U;
        colorCalculationRatioIndex = static_cast<std::uint8_t>((value >> 10U) & 0x3U);
        colorMask = 0x01FFU;
        break;
    default:
        // The 8-bit framebuffer sprite types are not selected in 16-bit TVM.
        // Keep a bounded palette interpretation until that framebuffer mode
        // has its own byte-pitch and sprite-type path.
        colorMask = 0x07FFU;
        break;
    }
    priority = priorityForIndex(priorityIndex);
    if (priority == 0U) return 0U;

    const auto colorCode = static_cast<std::size_t>(value & colorMask);
    const auto spriteColorOffset = static_cast<std::size_t>(
        (vdp2Register(0x0E6U) >> 4U) & 0x7U);
    const auto cramMode = static_cast<unsigned>((vdp2Register(0x00EU) >> 12U) & 0x3U);
    const auto offsetStride = cramMode == 2U ? 0x400U : 0x200U;
    auto cramAddress = (colorCode + spriteColorOffset * offsetStride) & 0x07FFU;
    if (cramMode == 0U || cramMode == 2U) cramAddress &= 0x03FFU;
    const auto cramColor = vdp2CramWord(cramAddress * 2U);
    return addVdp2Transparency(cramColor);
}

std::uint8_t VideoRuntime::colorCalculationRatioForLayer(
    std::uint8_t layer) const noexcept {
    std::uint32_t registerOffset{};
    unsigned shift{};
    switch (layer) {
    case kVdp2LayerNbg0: registerOffset = 0x108U; break;
    case kVdp2LayerNbg1: registerOffset = 0x108U; shift = 8U; break;
    case kVdp2LayerNbg2: registerOffset = 0x10AU; break;
    case kVdp2LayerNbg3: registerOffset = 0x10AU; shift = 8U; break;
    case kVdp2LayerRbg0: registerOffset = 0x10CU; break;
    case kVdp2LayerBack: registerOffset = 0x10EU; shift = 8U; break;
    default: return 0U;
    }
    return static_cast<std::uint8_t>((vdp2Register(registerOffset) >> shift) & 0x1FU);
}

std::uint32_t VideoRuntime::resolveColorCalculation(
    const Vdp2CompositePixel& pixel) const noexcept {
    if ((pixel.flags & kVdp2PixelTopColorCalculation) == 0U) {
        return pixel.topColor;
    }
    const auto colorControl = vdp2Register(0x0ECU);
    const auto ratio = (colorControl & 0x0200U) != 0U
        ? pixel.secondRatio : pixel.topRatio;
    auto second = (pixel.flags & kVdp2PixelSecondValid) != 0U
        ? pixel.secondColor : 0xFF000000U;
    if ((colorControl & 0x0400U) != 0U &&
        (pixel.flags & (kVdp2PixelSecondScreenColorCalculation |
                        kVdp2PixelThirdValid)) ==
            (kVdp2PixelSecondScreenColorCalculation | kVdp2PixelThirdValid)) {
        // With no inserted line-color screen, EXCCEN forms the second image
        // from a 1/2 blend of the second and third images when the second
        // image's CCEN is set (VDP2 User's Manual, Table 12.2).
        second = calculateVdp2Color(second, pixel.thirdColor, 15U, false);
    }
    return calculateVdp2Color(pixel.topColor, second, ratio,
                              (colorControl & 0x0100U) != 0U);
}

void VideoRuntime::composeVdp2Back(std::vector<std::uint32_t>& pixels) {
    // BACK is the fallback surface underneath enabled scroll screens. Yabause
    // follows the hardware publication rule: if both DISP and BDCLMD are
    // clear, the output is black and the table is not consumed. Otherwise the
    // register selects a single RGB555 word or one word per display line.
    const auto tvmd = vdp2Register(0x000U);
    const auto published = (tvmd & 0x8000U) != 0U || (tvmd & 0x0100U) != 0U;
    if (!published) {
        std::fill(pixels.begin(), pixels.end(), 0xFF000000U);
        return;
    }

    // The register stores a 19-bit word address. VRSIZE selects whether the
    // high two or three address bits are decoded, matching Yabause's
    // Vdp2DrawBackScreen path.
    const auto bktau = vdp2Register(0x0ACU);
    const auto bktal = vdp2Register(0x0AEU);
    const auto vramSize = vdp2Register(0x006U);
    const auto upperMask = (vramSize & 0x8000U) != 0U ? 0x7U : 0x3U;
    const auto address = static_cast<std::size_t>(((bktau & upperMask) << 16U) |
                                                   bktal) * 2U;
    const auto lineMode = (bktau & 0x8000U) != 0U;
    for (std::uint32_t y = 0; y < frameHeight(); ++y) {
        const auto line = lineMode ? y : 0U;
        const auto color = addVdp2Transparency(
            vdp2VramWord(address + static_cast<std::size_t>(line) * 2U));
        const auto opaque = color == 0U ? 0xFF000000U : color;
        std::fill_n(pixels.begin() + static_cast<std::size_t>(y) * kFrameWidth,
                    kFrameWidth, opaque);
    }
}

void VideoRuntime::composeVdp2RotationBitmap(
    std::vector<std::uint32_t>& pixels,
    std::vector<std::uint8_t>* colorCalculationFlags) {
    const auto bgon = vdp2Register(0x020U);
    const auto characterControl = vdp2Register(0x02AU);
    const auto bitmapPalette = vdp2Register(0x02EU);
    if ((bgon & 0x0010U) == 0U || (characterControl & 0x0200U) == 0U) return;

    // RBG0 rotation parameter A/B direct-color bitmap path. Cell maps,
    // coefficient-table perspective, and parameter-window switching still
    // use the CPU reference work planned separately.
    const auto colorMode = static_cast<unsigned>((characterControl >> 12U) & 0x7U);
    if (colorMode != 3U && colorMode != 4U) return;
    const auto rotationMode = static_cast<unsigned>(vdp2Register(0x0B0U) & 0x3U);
    if (rotationMode > 1U) return;
    const auto coefficientControl = vdp2Register(0x0B4U);
    if (((coefficientControl >> (rotationMode * 8U)) & 0x1U) != 0U) return;

    const auto rpta = (static_cast<std::uint32_t>(vdp2Register(0x0BCU) & 0x0007U) << 16U) |
                       (vdp2Register(0x0BEU) & 0xFFFEU);
    const auto parameterBase = ((rpta & 0x7FFBEU) + rotationMode * 0x40U) & 0x3FFFFU;
    const auto readLong = [this, parameterBase](std::uint32_t wordOffset) {
        const auto word = (parameterBase + wordOffset) & 0x3FFFFU;
        const auto nextWord = (word + 1U) & 0x3FFFFU;
        return (static_cast<std::uint32_t>(vdp2VramWord(word * 2U)) << 16U) |
            vdp2VramWord(nextWord * 2U);
    };
    const auto signedField = [](std::uint32_t value, unsigned width) {
        const auto sign = std::uint32_t{1U} << (width - 1U);
        const auto mask = (std::uint32_t{1U} << width) - 1U;
        value &= mask;
        const auto signedValue = static_cast<std::int32_t>(value);
        return (value & sign) != 0U
            ? signedValue - static_cast<std::int32_t>(mask + 1U) : signedValue;
    };
    const auto signedLong = [&](std::uint32_t wordOffset, unsigned width,
                                bool fractionalSix) {
        auto value = readLong(wordOffset);
        if (fractionalSix) value >>= 6U;
        return static_cast<std::int64_t>(signedField(value, width));
    };

    const auto xst = signedLong(0x00U, 23U, true);
    const auto yst = signedLong(0x02U, 23U, true);
    const auto zst = signedLong(0x04U, 23U, true);
    const auto dxst = signedLong(0x06U, 13U, true);
    const auto dyst = signedLong(0x08U, 13U, true);
    const auto dx = signedLong(0x0AU, 13U, true);
    const auto dy = signedLong(0x0CU, 13U, true);
    std::array<std::int64_t, 6> matrix{};
    for (unsigned index = 0U; index < matrix.size(); ++index) {
        matrix[index] = signedLong(0x0EU + index * 2U, 14U, true);
    }
    const auto signedWord = [this, parameterBase](std::uint32_t wordOffset) {
        const auto address = ((parameterBase + wordOffset) & 0x3FFFFU) * 2U;
        const auto value = static_cast<std::int32_t>(
            vdp2VramWord(address) & 0x3FFFU);
        return static_cast<std::int64_t>((value & 0x2000) != 0
            ? value - 0x4000 : value);
    };
    const auto px = signedWord(0x1AU);
    const auto py = signedWord(0x1BU);
    const auto pz = signedWord(0x1CU);
    const auto cx = signedWord(0x1EU);
    const auto cy = signedWord(0x1FU);
    const auto cz = signedWord(0x20U);
    const auto mx = signedLong(0x22U, 24U, true);
    const auto my = signedLong(0x24U, 24U, true);
    const auto kx = signedLong(0x26U, 24U, false);
    const auto ky = signedLong(0x28U, 24U, false);

    const auto bitmapSize = static_cast<unsigned>((characterControl >> 10U) & 0x1U);
    constexpr std::uint32_t width = 512U;
    const auto height = bitmapSize != 0U ? 512U : 256U;
    const auto bitsPerPixel = colorMode == 3U ? 16U : 32U;
    const auto rowPitch = static_cast<std::size_t>(width) * bitsPerPixel / 8U;
    const auto mapOffset = static_cast<std::size_t>(
        (vdp2Register(0x03EU) >> (rotationMode * 4U)) & 0x7U);
    const auto bitmapBase = mapOffset * 0x20000U;
    const auto planeSize = vdp2Register(0x03AU);
    const auto screenOver = static_cast<unsigned>(
        (planeSize >> (10U + rotationMode * 4U)) & 0x3U);
    const auto ignoreTransparent = (bgon & 0x1000U) != 0U;
    const auto bitmapColorCalculation = (bitmapPalette & 0x0010U) != 0U;
    const auto specialColorMode = static_cast<unsigned>(
        (vdp2Register(0x0EEU) >> 8U) & 0x3U);
    const auto specialColorControl = (vdp2Register(0x0ECU) & 0x0010U) != 0U;
    const bool windowsEnabled = (vdp2Register(0x0D4U) & 0x000AU) != 0U;

    for (std::uint32_t y = 0U; y < frameHeight(); ++y) {
        const auto xstAccum = xst + static_cast<std::int64_t>(y) * dxst;
        const auto ystAccum = yst + static_cast<std::int64_t>(y) * dyst;
        const auto xsp = (matrix[0] * (xstAccum - px * 1024) +
                          matrix[1] * (ystAccum - py * 1024) +
                          matrix[2] * (zst - pz * 1024)) >> 10U;
        const auto ysp = (matrix[3] * (xstAccum - px * 1024) +
                          matrix[4] * (ystAccum - py * 1024) +
                          matrix[5] * (zst - pz * 1024)) >> 10U;
        const auto xp = matrix[0] * (px - cx) + matrix[1] * (py - cy) +
                        matrix[2] * (pz - cz) + cx * 1024 + mx;
        const auto yp = matrix[3] * (px - cx) + matrix[4] * (py - cy) +
                        matrix[5] * (pz - cz) + cy * 1024 + my;
        const auto xStep = (matrix[0] * dx + matrix[1] * dy) >> 10U;
        const auto yStep = (matrix[3] * dx + matrix[4] * dy) >> 10U;
        for (std::uint32_t x = 0U; x < kFrameWidth; ++x) {
            if (windowsEnabled && !vdp2WindowAllows(4U, x, y)) continue;
            const auto sourceX = (xp + ((kx * (xsp + xStep * x)) >> 16U)) >> 10U;
            const auto sourceY = (yp + ((ky * (ysp + yStep * x)) >> 16U)) >> 10U;
            const auto outside = sourceX < 0 || sourceY < 0 ||
                sourceX >= width || sourceY >= height;
            if (outside && screenOver == 2U) continue;
            const auto wrappedX = static_cast<std::uint32_t>(sourceX) & (width - 1U);
            const auto wrappedY = static_cast<std::uint32_t>(sourceY) & (height - 1U);
            const auto address = bitmapBase + static_cast<std::size_t>(wrappedY) * rowPitch +
                static_cast<std::size_t>(wrappedX) * bitsPerPixel / 8U;
            std::uint32_t color{};
            bool specialPixel{};
            if (bitsPerPixel == 16U) {
                const auto raw = vdp2VramWord(address & (kVdp2VramSize - 2U));
                if (ignoreTransparent || (raw & 0x8000U) != 0U) {
                    color = rgb555ToArgb(raw);
                    specialPixel = (raw & 0x8000U) != 0U;
                }
            } else {
                const auto raw = (static_cast<std::uint32_t>(
                    vdp2VramWord(address & (kVdp2VramSize - 2U))) << 16U) |
                    vdp2VramWord((address + 2U) & (kVdp2VramSize - 2U));
                if (ignoreTransparent || (raw & 0x00FFFFFFU) != 0U) {
                    color = rgb888ToArgb(raw);
                }
                specialPixel = (raw & 0x80000000U) != 0U;
            }
            if (color == 0U) continue;
            const auto outputIndex = static_cast<std::size_t>(y) * kFrameWidth + x;
            pixels[outputIndex] = color;
            if (colorCalculationFlags != nullptr && specialColorControl &&
                ((specialColorMode == 1U && bitmapColorCalculation) ||
                 (specialColorMode == 3U && specialPixel))) {
                (*colorCalculationFlags)[outputIndex] = 1U;
            }
        }
    }
}

void VideoRuntime::composeVdp2(
    std::vector<std::uint32_t>& pixels,
    std::array<VideoFrame, 4>* layerFrames, bool resolveOutput) {
    vdp2CompositePixels_.assign(pixels.size(), Vdp2CompositePixel{});
    const auto tvmd = vdp2Register(0x000U);
    if (mapReadTraceEnabled_) {
        VideoMapStateRecord state{};
        state.frame = frameCount_;
        state.contextStep = contextStep_;
        state.contextBlock = contextBlock_;
        state.contextCaller = contextCaller_;
        state.tvmd = tvmd;
        state.bgon = vdp2Register(0x020U);
        state.chctla = vdp2Register(0x028U);
        state.mpofn = vdp2Register(0x03CU);
        state.mapAB = vdp2Register(0x040U);
        state.nbg0Enabled = (state.bgon & kVdp2Nbg0Enable) != 0U;
        state.cellMode = (state.chctla & 0x0002U) == 0U;
        state.expectedWords = (0x72000U - 0x70000U) / 2U;
        state.expectedFirstNonZeroAddress = kVdp2VramSize;
        for (std::size_t address = 0x70000U; address < 0x72000U; address += 2U) {
            const auto value = vdp2VramWord(address);
            if (value == 0U) continue;
            ++state.expectedNonZeroWords;
            if (state.expectedFirstNonZeroAddress == kVdp2VramSize) {
                state.expectedFirstNonZeroAddress = address;
                state.expectedFirstNonZeroValue = value;
            }
        }
        mapStateTrace_.push_back(state);
    }
    if ((tvmd & kVdp2DisplayEnable) == 0U) {
        for (auto& pixel : vdp2CompositePixels_) {
            pixel.topColor = 0xFF000000U;
            pixel.topLayer = kVdp2LayerBack;
        }
        return;
    }

    composeVdp2Back(pixels);
    const auto backRatio = colorCalculationRatioForLayer(kVdp2LayerBack);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        auto& composite = vdp2CompositePixels_[index];
        composite.topColor = pixels[index];
        composite.topLayer = kVdp2LayerBack;
        composite.topRatio = backRatio;
    }

    const auto bgon = vdp2Register(0x020U);
    const auto chctla = vdp2Register(0x028U);
    const auto bmpna = vdp2Register(0x02CU);
    const auto mpofn = vdp2Register(0x03CU);
    const auto prina = vdp2Register(0x0F8U);
    const auto prinb = vdp2Register(0x0FAU);
    const auto colorControl = vdp2Register(0x0ECU);
    const auto specialColorMode = vdp2Register(0x0EEU);
    // A freshly reset VDP2 has no priority programming yet. Keep the runtime
    // compatible with callers that only enable a layer (the legacy tests and
    // early boot code), while honoring explicit zero priorities as disabled.
    const auto n0Priority = vdp2PriorityConfigured_
        ? static_cast<unsigned>(prina & 0x7U)
        : ((bgon & kVdp2Nbg0Enable) != 0U ? 1U : 0U);
    const auto n1Priority = vdp2PriorityConfigured_
        ? static_cast<unsigned>((prina >> 8U) & 0x7U)
        : ((bgon & kVdp2Nbg1Enable) != 0U ? 1U : 0U);
    const auto n2Priority = vdp2PriorityConfigured_
        ? static_cast<unsigned>(prinb & 0x7U)
        : ((bgon & kVdp2Nbg2Enable) != 0U ? 1U : 0U);
    const auto n3Priority = vdp2PriorityConfigured_
        ? static_cast<unsigned>((prinb >> 8U) & 0x7U)
        : ((bgon & kVdp2Nbg3Enable) != 0U ? 1U : 0U);
    const auto rbg0Priority = vdp2PriorityConfigured_
        ? static_cast<unsigned>(vdp2Register(0x0FCU) & 0x7U)
        : ((bgon & 0x0010U) != 0U ? 1U : 0U);

    // Render each scroll surface into a transparent scratch frame and publish
    // it through a per-pixel priority buffer. This preserves BACK wherever a
    // layer is transparent, ignores priority-zero layers, and resolves equal
    // priorities in hardware order (NBG0 is above NBG1, NBG2, and NBG3).
    // Alpha zero is the empty-dot sentinel; opaque black is a valid palette
    // color and must participate in priority and color calculation.
    const auto transparent = 0U;
    if (layerFrames != nullptr) {
        for (auto& frame : *layerFrames) {
            frame.width = kFrameWidth;
            frame.height = frameHeight();
            frame.sequence = frameCount_;
            frame.pixels.assign(pixels.size(), transparent);
        }
    }
    const auto mergeLayer = [&](const std::vector<std::uint32_t>& layer,
                                const std::vector<std::uint8_t>& calculationFlags,
                                unsigned layerPriority, std::uint8_t layerIndex,
                                std::uint8_t compositeLayer,
                                unsigned specialPriorityMode) {
        if (layerPriority == 0U) return;
        const auto layerRatio = colorCalculationRatioForLayer(compositeLayer);
        const auto specialMode = static_cast<unsigned>(
            (specialColorMode >> (layerIndex * 2U)) & 0x3U);
        const auto screenColorCalculationEnabled =
            (colorControl & (1U << layerIndex)) != 0U;
        const auto moveSecondToThird = [](Vdp2CompositePixel& composite) {
            if ((composite.flags & kVdp2PixelSecondValid) != 0U) {
                composite.thirdColor = composite.secondColor;
                composite.flags |= kVdp2PixelThirdValid;
            } else {
                composite.flags &= static_cast<std::uint8_t>(~kVdp2PixelThirdValid);
            }
        };
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            if (layer[index] == transparent) continue;
            auto& composite = vdp2CompositePixels_[index];
            const auto colorCalculationEnabled = screenColorCalculationEnabled &&
                (specialMode == 0U || calculationFlags[index] != 0U);
            // The upper two priority bits always come from the priority
            // register; the special priority function only replaces the least
            // significant bit, and a resulting priority of zero is transparent.
            auto priority = static_cast<std::uint8_t>(layerPriority);
            if (specialPriorityMode != 0U && index < vdp2SpecialScratch_.size()) {
                priority = static_cast<std::uint8_t>(
                    (layerPriority & ~1U) | (vdp2SpecialScratch_[index] != 0U ? 1U : 0U));
                if (priority == 0U) continue;
            }
            if (isAboveVdp2Candidate(priority, compositeLayer,
                                     composite.topPriority, composite.topLayer)) {
                const auto oldTopScreenColorCalculation =
                    (composite.flags & kVdp2PixelTopScreenColorCalculation) != 0U;
                moveSecondToThird(composite);
                composite.secondColor = composite.topColor;
                composite.secondPriority = composite.topPriority;
                composite.secondLayer = composite.topLayer;
                composite.secondRatio = composite.topRatio;
                composite.topColor = layer[index];
                composite.topPriority = priority;
                composite.topLayer = compositeLayer;
                composite.topRatio = layerRatio;
                composite.flags = static_cast<std::uint8_t>(
                    (composite.flags | kVdp2PixelSecondValid) &
                    ~(kVdp2PixelTopColorCalculation |
                      kVdp2PixelTopScreenColorCalculation |
                      kVdp2PixelSecondScreenColorCalculation));
                if (oldTopScreenColorCalculation) {
                    composite.flags |= kVdp2PixelSecondScreenColorCalculation;
                }
                if (colorCalculationEnabled) {
                    composite.flags |= kVdp2PixelTopColorCalculation;
                }
                if (screenColorCalculationEnabled) {
                    composite.flags |= kVdp2PixelTopScreenColorCalculation;
                }
            } else if ((composite.flags & kVdp2PixelSecondValid) == 0U ||
                       isAboveVdp2Candidate(priority, compositeLayer,
                                            composite.secondPriority,
                                            composite.secondLayer)) {
                moveSecondToThird(composite);
                composite.secondColor = layer[index];
                composite.secondPriority = priority;
                composite.secondLayer = compositeLayer;
                composite.secondRatio = layerRatio;
                composite.flags |= kVdp2PixelSecondValid;
                composite.flags &= static_cast<std::uint8_t>(
                    ~kVdp2PixelSecondScreenColorCalculation);
                if (screenColorCalculationEnabled) {
                    composite.flags |= kVdp2PixelSecondScreenColorCalculation;
                }
            }
            pixels[index] = composite.topColor;
        }
    };
    const auto prepareLayer = [&] {
        vdp2LayerScratch_.assign(pixels.size(), transparent);
        vdp2CalculationScratch_.assign(pixels.size(), 0U);
        vdp2SpecialScratch_.assign(pixels.size(), 0U);
    };
    auto& layer = vdp2LayerScratch_;
    auto& calculationFlags = vdp2CalculationScratch_;
    // SFPRMD selects the special priority mode per scroll screen: 0 keeps the
    // priority register value, 1 takes the least significant bit from the
    // pattern name data and 2 only for the dots that match the special
    // function code (VDP2 User's Manual 11.2).
    const auto specialPriorityModeRegister = vdp2Register(0x0EAU);
    const auto layerSpecialMode = [&](unsigned layerIndex) {
        return static_cast<unsigned>((specialPriorityModeRegister >> (layerIndex * 2U)) & 0x3U);
    };
    // Diagnostic layer captures and map traces must still see disabled layers.
    const auto skipZeroPriority = layerFrames == nullptr &&
        !mapReadTraceEnabled_ && !(diagnosticCaptureEnabled_ && diagnosticSink_);
    const auto drawNbg0 = [&] {
        if ((bgon & kVdp2Nbg0Enable) == 0U) return;
        if (skipZeroPriority && n0Priority == 0U) return;
        prepareLayer();
        if ((chctla & 0x0002U) != 0U) {
            composeVdp2Bitmap(layer, 0U, chctla, bmpna, mpofn,
                              vdp2Register(0x070U), vdp2Register(0x074U),
                              &calculationFlags);
        } else {
            composeVdp2Cell(layer, 0U, chctla, vdp2Register(0x030U), mpofn,
                            vdp2Register(0x040U), vdp2Register(0x042U),
                            vdp2Register(0x070U), vdp2Register(0x074U),
                            nullptr, &calculationFlags, &vdp2SpecialScratch_);
        }
        if (layerFrames != nullptr) (*layerFrames)[0].pixels = layer;
        mergeLayer(layer, calculationFlags, n0Priority, kVdp2LayerNbg0,
                   kVdp2LayerNbg0, layerSpecialMode(0U));
    };
    const auto drawNbg1 = [&] {
        if ((bgon & kVdp2Nbg1Enable) == 0U) return;
        if (skipZeroPriority && n1Priority == 0U) return;
        // NBG0's 16M-color mode consumes the fetch slots of NBG1/2/3,
        // even when their BGON bits and priorities remain programmed.
        if ((bgon & kVdp2Nbg0Enable) != 0U &&
            ((chctla >> 4U) & 0x7U) == 4U) return;
        prepareLayer();
        if ((chctla & 0x0200U) != 0U) {
            composeVdp2Bitmap(layer, 1U, chctla, bmpna, mpofn,
                              vdp2Register(0x080U), vdp2Register(0x084U),
                              &calculationFlags);
        } else {
            composeVdp2Cell(layer, 1U, chctla, vdp2Register(0x032U), mpofn,
                            vdp2Register(0x044U), vdp2Register(0x046U),
                            vdp2Register(0x080U), vdp2Register(0x084U),
                            nullptr, &calculationFlags, &vdp2SpecialScratch_);
        }
        if (layerFrames != nullptr) (*layerFrames)[1].pixels = layer;
        mergeLayer(layer, calculationFlags, n1Priority, kVdp2LayerNbg1,
                   kVdp2LayerNbg1, layerSpecialMode(1U));
    };
    const auto drawNbg2 = [&] {
        if ((bgon & kVdp2Nbg2Enable) == 0U) return;
        if (skipZeroPriority && n2Priority == 0U) return;
        if ((bgon & kVdp2Nbg0Enable) != 0U &&
            ((chctla >> 4U) & 0x7U) >= 2U) return;
        prepareLayer();
        composeVdp2Cell(layer, 2U, vdp2Register(0x02AU), vdp2Register(0x034U),
                        mpofn, vdp2Register(0x048U), vdp2Register(0x04AU),
                        vdp2Register(0x090U), vdp2Register(0x092U),
                        nullptr, &calculationFlags, &vdp2SpecialScratch_);
        if (layerFrames != nullptr) (*layerFrames)[2].pixels = layer;
        mergeLayer(layer, calculationFlags, n2Priority, kVdp2LayerNbg2,
                   kVdp2LayerNbg2, layerSpecialMode(2U));
    };
    const auto drawNbg3 = [&] {
        if ((bgon & kVdp2Nbg3Enable) == 0U) return;
        if (skipZeroPriority && n3Priority == 0U) return;
        if (((bgon & kVdp2Nbg0Enable) != 0U &&
             ((chctla >> 4U) & 0x7U) == 4U) ||
            ((bgon & kVdp2Nbg1Enable) != 0U &&
             ((chctla >> 12U) & 0x3U) >= 2U)) return;
        prepareLayer();
        composeVdp2Cell(layer, 3U, vdp2Register(0x02AU), vdp2Register(0x036U),
                        mpofn, vdp2Register(0x04CU), vdp2Register(0x04EU),
                        vdp2Register(0x094U), vdp2Register(0x096U),
                        nullptr, &calculationFlags, &vdp2SpecialScratch_);
        if (layerFrames != nullptr) (*layerFrames)[3].pixels = layer;
        mergeLayer(layer, calculationFlags, n3Priority, kVdp2LayerNbg3,
                   kVdp2LayerNbg3, layerSpecialMode(3U));
    };
    const auto drawRbg0 = [&] {
        if ((bgon & 0x0010U) == 0U || rbg0Priority == 0U) return;
        prepareLayer();
        composeVdp2RotationBitmap(layer, &calculationFlags);
        mergeLayer(layer, calculationFlags, rbg0Priority, 4U, kVdp2LayerRbg0, 0U);
    };
    drawNbg3();
    drawNbg2();
    drawNbg1();
    drawNbg0();
    drawRbg0();
    if (resolveOutput) {
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            pixels[index] = resolveColorCalculation(vdp2CompositePixels_[index]);
        }
    }
}

void VideoRuntime::composeVdp2Bitmap(std::vector<std::uint32_t>& pixels,
                                     unsigned layer,
                                     std::uint16_t characterControl,
                                     std::uint16_t bitmapPalette,
                                     std::uint16_t mapOffset,
                                     std::uint16_t scrollX,
                                     std::uint16_t scrollY,
                                     std::vector<std::uint8_t>* colorCalculationFlags) {
    const auto windowControl = vdp2Register(layer < 2U ? 0x0D0U : 0x0D2U);
    const bool windowsEnabled = (windowControl & (0x000AU << ((layer & 1U) * 8U))) != 0U;
    const auto shift = layer == 0U ? 0U : 10U;
    const auto colorShift = layer == 0U ? 4U : 12U;
    const auto bitmapEnable = static_cast<std::uint16_t>(1U << (layer == 0U ? 1U : 9U));
    const auto colors = (characterControl >> colorShift) & (layer == 0U ? 0x7U : 0x3U);
    const auto palette = layer == 0U ? (bitmapPalette & 0x7U)
                                     : ((bitmapPalette >> 8U) & 0x7U);
    const auto specialMode = static_cast<unsigned>(
        (vdp2Register(0x0EEU) >> (layer * 2U)) & 0x3U);
    const auto bitmapSpecialColorCalculation =
        (bitmapPalette & (layer == 0U ? 0x0010U : 0x1000U)) != 0U;
    const auto specialFunctionSelect = vdp2Register(0x024U);
    const auto specialFunctionCodes = vdp2Register(0x026U);
    const auto base = static_cast<std::size_t>(
        ((mapOffset >> (layer == 0U ? 0U : 4U)) & 0x7U) * 0x20000U);
    const auto scrollXValue = static_cast<std::size_t>(scrollX & 0x07FFU);
    const auto scrollYValue = static_cast<std::size_t>(scrollY & 0x07FFU);
    const auto scrollRegisterBase = layer == 0U ? 0x070U : 0x080U;
    const auto scrollXFixed =
        (static_cast<std::uint32_t>(scrollXValue) << 16U) |
        (static_cast<std::uint32_t>(vdp2Register(scrollRegisterBase + 0x02U)) &
         0xFF00U);
    const auto scrollYFixed =
        (static_cast<std::uint32_t>(scrollYValue) << 16U) |
        (static_cast<std::uint32_t>(vdp2Register(scrollRegisterBase + 0x06U)) &
         0xFF00U);

    // Bitmap mode is selected by the layer's BMEN bit. For a cell-mode layer,
    // use a deterministic linear RGB555 fallback; this keeps resource DMA
    // useful before the full pattern-name/map decoder is introduced.
    const auto bitmap = (characterControl & bitmapEnable) != 0U;
    auto width = 320U;
    auto height = 224U;
    if (bitmap) {
        const auto size = (characterControl >> (shift + 2U)) & 0x3U;
        width = 512U;
        if ((size & 0x2U) != 0U) width = 1024U;
        height = (size & 0x1U) != 0U ? 512U : 256U;
    }
    // Bitmap color modes use 4/8/16/16/32 bits per pixel.  Keeping the
    // pitch in bits is important for the 4bpp and 8bpp palette modes: the
    // Saturn bitmap row is packed, rather than padded to one word per pixel.
    const auto bitsPerPixel = bitmap
        ? (colors == 0U ? 4U : colors == 1U ? 8U : colors == 2U ? 16U
            : colors == 3U ? 16U : colors == 4U ? 32U : 16U)
        : 16U;
    const auto rowPitch = (static_cast<std::size_t>(width) * bitsPerPixel + 7U) / 8U;

    // Saturn games commonly use the VDP2 continuous-bitmap trick: CHCTLA
    // remains 512x256/32bpp, while NBG0 line scroll and vertical-cell scroll
    // make a smaller (for example 320x224) image occupy contiguous VRAM.
    // Read the two fixed-point values exactly as the hardware does before
    // converting them to bitmap coordinates.  Other bitmap modes retain the
    // simple linear path below.
    const bool continuousNbg0 = bitmap && layer == 0U &&
        (vdp2Register(0x09AU) & 0x0007U) != 0U;
    const auto scrollControl = vdp2Register(0x09AU);
    const auto combineTableAddress = [](std::uint16_t upper,
                                        std::uint16_t lower) {
        return static_cast<std::size_t>(((upper & 0x0007U) << 16U) |
                                         (lower & 0xFFFEU)) * 2U;
    };
    const auto lineTableAddress = continuousNbg0
        ? combineTableAddress(vdp2Register(0x0A0U), vdp2Register(0x0A2U)) : 0U;
    const auto cellTableAddress = continuousNbg0
        ? combineTableAddress(vdp2Register(0x09CU), vdp2Register(0x09EU)) : 0U;
    const auto readVdp2Long = [this](std::size_t address) {
        return (static_cast<std::uint32_t>(vdp2VramWord(address)) << 16U) |
            vdp2VramWord(address + 2U);
    };
    const auto n0LineInterval = continuousNbg0
        ? (1U << ((scrollControl >> 4U) & 0x3U)) : 1U;
    const auto lineScrollEnabled = continuousNbg0 &&
        (scrollControl & 0x0002U) != 0U;
    const auto verticalLineScrollEnabled = continuousNbg0 &&
        (scrollControl & 0x0004U) != 0U;
    const auto verticalCellScrollEnabled = continuousNbg0 &&
        (scrollControl & 0x0001U) != 0U;
    const auto vdp2LineOrigin =
        (vdp2Register(0x000U) & 0x0030U) == 0x0010U ? 8U : 0U;
    std::size_t cellScrollStride = 4U;
    std::size_t cellScrollBias = 0U;
    if (verticalCellScrollEnabled) {
        std::size_t directNonZero{};
        std::size_t interleavedNonZero{};
        for (std::size_t cell = 1U; cell < 32U; ++cell) {
            if (readVdp2Long(cellTableAddress + cell * 4U) != 0U) {
                ++directNonZero;
            }
            if (readVdp2Long(cellTableAddress + 4U + cell * 8U) != 0U) {
                ++interleavedNonZero;
            }
        }
        if (interleavedNonZero >= 4U &&
            interleavedNonZero > directNonZero) {
            cellScrollStride = 8U;
            cellScrollBias = 4U;
        }
    }
    const auto readVerticalCell = [&, cellScrollStride, cellScrollBias](
                                       std::size_t cell) {
        return readVdp2Long(cellTableAddress + cellScrollBias +
                            cell * cellScrollStride);
    };

    for (std::uint32_t y = 0; y < frameHeight(); ++y) {
        std::size_t lineHorizontal = scrollXFixed >> 16U;
        std::uint32_t lineVertical = scrollYFixed;
        if (continuousNbg0) {
            const auto lineIndex = (static_cast<std::size_t>(y) +
                                    vdp2LineOrigin) / n0LineInterval;
            const auto lineAddress = lineTableAddress + lineIndex * 8U;
            if (lineScrollEnabled) {
                lineHorizontal = ((scrollXFixed + readVdp2Long(lineAddress)) >>
                                  16U) & 0x01FFU;
            }
            if (verticalLineScrollEnabled) {
                lineVertical = scrollYFixed + readVdp2Long(lineAddress + 4U);
            }
        }
        for (std::uint32_t x = 0; x < kFrameWidth; ++x) {
            const auto sourceX = (static_cast<std::size_t>(x) + lineHorizontal) % width;
            std::size_t sourceY{};
            if (continuousNbg0) {
                auto vertical = lineVertical;
                if (verticalCellScrollEnabled) {
                    const auto cell = static_cast<std::size_t>(x) / 8U;
                    vertical += readVerticalCell(cell);
                }
                sourceY = (vertical >> 16U) % height;
            } else {
                sourceY = (static_cast<std::size_t>(y) + scrollYValue) % height;
            }
            const auto rowOffset = base + static_cast<std::size_t>(sourceY) * rowPitch;
            const auto offset = rowOffset +
                (static_cast<std::size_t>(sourceX) * bitsPerPixel) / 8U;
            std::uint32_t color{};
            std::uint16_t rawColorData{};
            unsigned dotColorCode{};
            const auto paletteFormat = colors <= 2U;
            if (bitmap && colors == 4U) {
                const auto high = vdp2VramWord(offset);
                const auto low = vdp2VramWord(offset + 2U);
                const auto raw = (static_cast<std::uint32_t>(high) << 16U) | low;
                color = (raw & 0x00FFFFFFU) == 0U ? 0U : rgb888ToArgb(raw);
            } else if (bitmap && colors == 3U) {
                rawColorData = vdp2VramWord(offset);
                color = addVdp2Transparency(rawColorData);
            } else if (bitmap && colors == 2U) {
                dotColorCode = vdp2VramWord(offset) & 0x07FFU;
                rawColorData = vdp2CramWord(
                    static_cast<std::size_t>(dotColorCode) * 2U);
                color = addVdp2Transparency(rawColorData);
            } else if (bitmap && colors == 1U) {
                const auto packed = vdp2VramWord(offset & ~1U);
                const auto index = (offset & 1U) == 0U ? packed >> 8U : packed & 0xFFU;
                dotColorCode = index;
                rawColorData = vdp2CramWord(
                    (static_cast<std::size_t>(palette) << 8U | index) * 2U);
                color = addVdp2Transparency(rawColorData);
            } else if (bitmap && colors == 0U) {
                const auto packed = vdp2VramWord(offset & ~1U);
                const auto byte = (offset & 1U) == 0U ? packed >> 8U : packed & 0xFFU;
                const auto index = (sourceX & 1U) == 0U ? byte >> 4U : byte & 0x0FU;
                dotColorCode = index;
                rawColorData = vdp2CramWord(
                    (static_cast<std::size_t>(palette) << 4U | index) * 2U);
                color = addVdp2Transparency(rawColorData);
            } else {
                color = addVdp2Transparency(vdp2VramWord(offset));
            }
            if (color != 0U && (!windowsEnabled || vdp2WindowAllows(layer, x, y))) {
                const auto outputIndex = static_cast<std::size_t>(y) * kFrameWidth + x;
                pixels[outputIndex] = color;
                if (colorCalculationFlags != nullptr && specialMode != 0U) {
                    const auto specialEnabled = specialMode == 1U
                        ? bitmapSpecialColorCalculation
                        : specialMode == 2U
                            ? paletteFormat && bitmapSpecialColorCalculation &&
                                vdp2SpecialColorCodeMatches(
                                    specialFunctionSelect, specialFunctionCodes,
                                    layer, dotColorCode)
                            : !paletteFormat || (rawColorData & 0x8000U) != 0U;
                    (*colorCalculationFlags)[outputIndex] =
                        specialEnabled ? 1U : 0U;
                }
            }
        }
    }
}

void VideoRuntime::composeVdp2Cell(std::vector<std::uint32_t>& pixels, unsigned layer,
                                   std::uint16_t characterControl,
                                   std::uint16_t patternControl,
                                   std::uint16_t mapOffset,
                                   std::uint16_t mapAB, std::uint16_t mapCD,
                                   std::uint16_t scrollX, std::uint16_t scrollY,
                                   std::vector<VideoCellSample>* samples,
                                   std::vector<std::uint8_t>* colorCalculationFlags,
                                   std::vector<std::uint8_t>* specialPriority) {
    // Special priority (SFPRMD): modes 1 and 2 take the least significant bit
    // of the priority number from the pattern name data (mode 2 only for the
    // dots whose colour code matches the special function code). Mode 0 keeps
    // the priority register value untouched.
    const auto specialPriorityMode = static_cast<unsigned>(
        (vdp2Register(0x0EAU) >> (layer * 2U)) & 0x3U);
    const auto specialPrioritySelect = vdp2Register(0x024U);
    const auto specialPriorityCodes = vdp2Register(0x026U);
    const auto windowControl = vdp2Register(layer < 2U ? 0x0D0U : 0x0D2U);
    const bool windowsEnabled = (windowControl & (0x000AU << ((layer & 1U) * 8U))) != 0U;
    const auto colors = layer == 0U ? (characterControl >> 4U) & 0x7U
        : layer == 1U ? (characterControl >> 12U) & 0x3U
        : layer == 2U ? (characterControl >> 1U) & 0x1U
                      : (characterControl >> 5U) & 0x1U;
    const auto bitsPerPixel = colors == 0U ? 4U : colors == 1U ? 8U :
        colors == 2U ? 16U : colors == 3U ? 16U : colors == 4U ? 32U : 16U;
    const auto oneWord = (patternControl & 0x8000U) != 0U;
    const auto auxMode = (patternControl & 0x4000U) != 0U;
    const auto auxPalette = static_cast<unsigned>((patternControl >> 5U) & 0x7U);
    const auto auxCharacter = static_cast<unsigned>(patternControl & 0x1FU);
    const auto transparentDots = (vdp2Register(0x020U) & (0x0100U << layer)) == 0U;
    const auto colorOffset = static_cast<unsigned>(
        (vdp2Register(0x0E4U) >> (layer * 4U)) & 0x7U) << 8U;
    const auto specialMode = static_cast<unsigned>(
        (vdp2Register(0x0EEU) >> (layer * 2U)) & 0x3U);
    const auto specialFunctionSelect = vdp2Register(0x024U);
    const auto specialFunctionCodes = vdp2Register(0x026U);
    const auto doubleCellBit = layer == 0U ? 0U : layer == 1U ? 8U
        : layer == 2U ? 0U : 4U;
    const auto doubleCell = (characterControl & (1U << doubleCellBit)) != 0U;
    const auto pageSize = [&] {
        const auto size = (vdp2Register(0x03AU) >> (layer * 2U)) & 0x3U;
        return size == 0U ? std::pair<unsigned, unsigned>{1U, 1U}
             : size == 1U ? std::pair<unsigned, unsigned>{2U, 1U}
                          : std::pair<unsigned, unsigned>{2U, 2U};
    }();
    const auto pageBytes = static_cast<std::size_t>(
        (oneWord ? 0x2000U : 0x4000U) / (doubleCell ? 4U : 1U));
    const auto mapValue = [&](unsigned plane) {
        switch (plane) {
        case 0U: return static_cast<unsigned>(mapAB & 0x3FU);
        case 1U: return static_cast<unsigned>((mapAB >> 8U) & 0x3FU);
        case 2U: return static_cast<unsigned>(mapCD & 0x3FU);
        default: return static_cast<unsigned>((mapCD >> 8U) & 0x3FU);
        }
    };
    const auto offsetBits = static_cast<unsigned>(
        (mapOffset >> (layer * 4U)) & 0x7U);
    const auto plainSizeCode = (vdp2Register(0x03AU) >> (layer * 2U)) & 0x3U;
    const auto addressForPlane = [&](unsigned plane) {
        const auto value = (offsetBits << 6U) | mapValue(plane);
        // VDP2 manual, table 4.8: 16x16 characters use one quarter
        // as many pattern names per page. Low map bits align the plane.
        const auto alignment = plainSizeCode == 0U ? 0U :
            plainSizeCode == 1U ? 1U : 2U;
        return (static_cast<std::size_t>((value >> alignment) << alignment) *
                pageBytes) & (kVdp2VramSize - 1U);
    };
    // A/B/C/D are the four planes in a 2x2 map. PLSZ controls how many
    // consecutive 512x512 pages make up each plane; it does not turn each
    // page into a separate map plane.
    const auto planeWidth = static_cast<std::size_t>(pageSize.first) * 512U;
    const auto planeHeight = static_cast<std::size_t>(pageSize.second) * 512U;
    const auto planeWidthShift = pageSize.first == 2U ? 10U : 9U;
    const auto planeHeightShift = pageSize.second == 2U ? 10U : 9U;
    const auto screenWidth = planeWidth * 2U;
    const auto screenHeight = planeHeight * 2U;
    const std::array planeAddresses{addressForPlane(0U), addressForPlane(1U),
                                   addressForPlane(2U), addressForPlane(3U)};
    const auto cellBytes = static_cast<std::size_t>(8U * 8U * bitsPerPixel / 8U);
    const auto pntWords = oneWord ? 1U : 2U;
    const auto pntEntryBytes = static_cast<std::size_t>(pntWords) * 2U;
    const auto charWidth = doubleCell ? 16U : 8U;
    const auto charHeight = doubleCell ? 16U : 8U;
    const auto charShift = doubleCell ? 4U : 3U;
    const auto traceReads = mapReadTraceEnabled_ && layer == 0U;
    const bool packed4BitSpans = colors == 0U && !windowsEnabled &&
        samples == nullptr && !traceReads;
    VideoMapReadRecord readRecord{};
    if (traceReads) {
        readRecord.frame = frameCount_;
        readRecord.contextStep = contextStep_;
        readRecord.contextBlock = contextBlock_;
        readRecord.contextCaller = contextCaller_;
        readRecord.rendererMinAddress = kVdp2VramSize;
        readRecord.rendererMaxAddress = 0U;
        readRecord.rendererFirstNonZeroAddress = kVdp2VramSize;
        readRecord.expectedWords = (0x72000U - 0x70000U) / 2U;
        readRecord.expectedFirstNonZeroAddress = kVdp2VramSize;
    }
    const auto recordRendererWord = [&](std::size_t address,
                                        std::uint16_t value) {
        if (!traceReads) return;
        const auto physicalAddress = address & (kVdp2VramSize - 1U);
        ++readRecord.rendererReads;
        readRecord.rendererMinAddress =
            std::min(readRecord.rendererMinAddress, physicalAddress);
        readRecord.rendererMaxAddress =
            std::max(readRecord.rendererMaxAddress, physicalAddress);
        if (value == 0U) return;
        ++readRecord.rendererNonZeroReads;
        if (readRecord.rendererFirstNonZeroAddress == kVdp2VramSize) {
            readRecord.rendererFirstNonZeroAddress = physicalAddress;
            readRecord.rendererFirstNonZeroValue = value;
        }
    };

    for (std::uint32_t y = 0; y < frameHeight(); ++y) {
        // Map and character dimensions are powers of two. Compute row
        // coordinates once and avoid integer division for every dot.
        const auto sourceY = (static_cast<std::size_t>(y) + (scrollY & 0x07FFU)) & (screenHeight - 1U);
        const auto planeY = static_cast<unsigned>(sourceY >> planeHeightShift);
        const auto pageY = (sourceY & (planeHeight - 1U)) >> 9U;
        const auto localY = sourceY & 0x1FFU;
        const auto patternY = localY >> charShift;
        // VRAM cannot change during this synchronous composition. A pattern
        // name is shared by the next 8/16 dots, but diagnostic reads retain
        // their original per-dot accounting below.
        auto cachedPntAddress = static_cast<std::size_t>(-1);
        std::uint16_t upper{}, lower{};
        unsigned palette{}, character{}, flip{};
        // Special priority bit from the pattern name data: bit 15 of the single
        // word, or bit 15 of the lower word in the two-word form.
        unsigned patternSpecialPriority{};
        for (std::uint32_t x = 0; x < kFrameWidth; ++x) {
            const auto sourceX = (static_cast<std::size_t>(x) + (scrollX & 0x07FFU)) & (screenWidth - 1U);
            const auto planeX = static_cast<unsigned>(sourceX >> planeWidthShift);
            const auto plane = planeY * 2U + planeX;
            const auto pageX = (sourceX & (planeWidth - 1U)) >> 9U;
            const auto pageIndex = pageY * pageSize.first + pageX;
            const auto localX = sourceX & 0x1FFU;
            const auto patternX = localX >> charShift;
            const auto pntAddress = planeAddresses[plane] + pageIndex * pageBytes +
                ((patternY << (9U - charShift)) + patternX) * pntEntryBytes;
            if (pntAddress != cachedPntAddress) {
                cachedPntAddress = pntAddress;
                upper = vdp2VramWord(pntAddress);
                lower = oneWord ? std::uint16_t{0} : vdp2VramWord(pntAddress + 2U);
                patternSpecialPriority = oneWord
                    ? static_cast<unsigned>((upper >> 15U) & 1U)
                    : static_cast<unsigned>((lower >> 15U) & 1U);
                if (oneWord) {
                    palette = colors == 0U
                        ? static_cast<unsigned>((upper >> 12U) & 0xFU) | (auxPalette << 4U)
                        : static_cast<unsigned>((upper >> 8U) & 0x70U);
                    flip = auxMode ? 0U : static_cast<unsigned>((upper >> 10U) & 3U);
                    character = static_cast<unsigned>(upper & (auxMode ? 0x0FFFU : 0x03FFU));
                    if (doubleCell) {
                        character = (character << 2U) | (auxCharacter & 3U) |
                            ((auxCharacter & (auxMode ? 0x10U : 0x1CU)) << 10U);
                    } else {
                        character |= (auxCharacter & (auxMode ? 0x1CU : 0x1FU)) << 10U;
                    }
                } else {
                    // In the two-word form the upper word carries palette bits
                    // 6..0; the lower word carries the 15-bit character number.
                    palette = static_cast<unsigned>(upper & (colors == 0U ? 0x007FU : 0x0070U));
                    character = static_cast<unsigned>(lower & 0x7FFFU);
                    flip = static_cast<unsigned>((upper >> 14U) & 3U);
                }
            }
            recordRendererWord(pntAddress, upper);
            if (!oneWord) recordRendererWord(pntAddress + 2U, lower);
            const auto inCharX = (flip & 1U) != 0U
                ? charWidth - 1U - (localX & (charWidth - 1U)) : localX & (charWidth - 1U);
            const auto inCharY = (flip & 2U) != 0U
                ? charHeight - 1U - (localY & (charHeight - 1U)) : localY & (charHeight - 1U);
            const auto cellX = inCharX / 8U;
            const auto cellY = inCharY / 8U;
            const auto dotX = inCharX & 7U;
            const auto dotY = inCharY & 7U;
            const auto cellIndex = cellY * (doubleCell ? 2U : 1U) + cellX;
            // Character numbers always address 32-byte units. A 16x16
            // character adds four cells to that base, without scaling it.
            const auto patternAddress = static_cast<std::size_t>(character) *
                0x20U + cellIndex * cellBytes;
            const auto rowAddress = patternAddress +
                (static_cast<std::size_t>(dotY) * 8U + dotX) * bitsPerPixel / 8U;
            if (packed4BitSpans) {
                // Stop at the next 8-dot boundary, including inside a 16x16
                // character. Horizontal flip reverses dots within this cell;
                // the next span recomputes the flipped subcell normally.
                const auto span = std::min<std::uint32_t>(
                    8U - static_cast<unsigned>(localX & 7U), kFrameWidth - x);
                const auto rowBase = patternAddress + static_cast<std::size_t>(dotY) * 4U;
                const auto packed = (static_cast<std::uint32_t>(vdp2VramWord(rowBase)) << 16U) |
                    vdp2VramWord(rowBase + 2U);
                const auto paletteBase = colorOffset + (palette << 4U);
                const auto outputBase = static_cast<std::size_t>(y) * kFrameWidth + x;
                const auto patternSpecialColorCalculation = oneWord
                    ? (patternControl & 0x0100U) != 0U : (upper & 0x1000U) != 0U;
                for (std::uint32_t n = 0U; n < span; ++n) {
                    const auto sourceDot = (flip & 1U) != 0U ? dotX - n : dotX + n;
                    const auto index = (packed >> ((7U - sourceDot) * 4U)) & 0xFU;
                    if (transparentDots && index == 0U) continue;
                    const auto rawColorData = vdp2CramWord(
                        static_cast<std::size_t>(paletteBase + index) * 2U);
                    pixels[outputBase + n] = rgb555ToArgb(rawColorData);
                    if (specialPriority != nullptr) {
                        const auto specialLsb = specialPriorityMode == 0U ||
                                patternSpecialPriority == 0U ? 0U
                            : specialPriorityMode == 1U ? 1U
                            : (vdp2SpecialColorCodeMatches(specialPrioritySelect,
                                                           specialPriorityCodes,
                                                           layer, index) ? 1U : 0U);
                        (*specialPriority)[outputBase + n] =
                            static_cast<std::uint8_t>(specialLsb);
                    }
                    if (colorCalculationFlags != nullptr && specialMode != 0U) {
                        // This path is always palette format; retain the same
                        // special-function policy as the generic pixel path.
                        const auto specialEnabled = specialMode == 1U
                            ? patternSpecialColorCalculation
                            : specialMode == 2U
                                ? patternSpecialColorCalculation && vdp2SpecialColorCodeMatches(
                                    specialFunctionSelect, specialFunctionCodes, layer, index)
                                : (rawColorData & 0x8000U) != 0U;
                        (*colorCalculationFlags)[outputBase + n] = specialEnabled ? 1U : 0U;
                    }
                }
                x += span - 1U;
                continue;
            }
            std::uint32_t color = 0U;
            std::size_t cramAddress = kVdp2CramSize;
            std::uint16_t rawColorData{};
            unsigned dotColorCode{};
            const auto paletteFormat = colors <= 2U;
            if (colors == 0U) {
                const auto packed = vdp2VramWord(rowAddress & ~1U);
                const auto byte = (rowAddress & 1U) == 0U ? packed >> 8U : packed & 0xFFU;
                const auto index = (dotX & 1U) == 0U ? byte >> 4U : byte & 0x0FU;
                dotColorCode = index;
                cramAddress = (colorOffset + ((palette << 4U) | index)) * 2U;
                rawColorData = vdp2CramWord(cramAddress);
                color = transparentDots && index == 0U ? 0U : rgb555ToArgb(rawColorData);
            } else if (colors == 1U) {
                const auto packed = vdp2VramWord(rowAddress & ~1U);
                const auto index = (rowAddress & 1U) == 0U ? packed >> 8U : packed & 0xFFU;
                dotColorCode = index;
                cramAddress = (colorOffset + ((palette << 4U) | index)) * 2U;
                rawColorData = vdp2CramWord(cramAddress);
                color = transparentDots && index == 0U ? 0U : rgb555ToArgb(rawColorData);
            } else if (colors == 2U) {
                dotColorCode = vdp2VramWord(rowAddress) & 0x07FFU;
                cramAddress = static_cast<std::size_t>(colorOffset + dotColorCode) * 2U;
                rawColorData = vdp2CramWord(cramAddress);
                color = transparentDots && dotColorCode == 0U ? 0U : rgb555ToArgb(rawColorData);
            } else if (colors == 3U) {
                const auto raw = vdp2VramWord(rowAddress);
                rawColorData = raw;
                color = transparentDots && (raw & 0x8000U) == 0U ? 0U : rgb555ToArgb(raw);
            } else if (colors == 4U) {
                const auto raw = (static_cast<std::uint32_t>(vdp2VramWord(rowAddress)) << 16U) |
                    vdp2VramWord(rowAddress + 2U);
                color = (raw & 0x00FFFFFFU) == 0U ? 0U : rgb888ToArgb(raw);
            } else {
                color = addVdp2Transparency(vdp2VramWord(rowAddress));
            }
            if (samples != nullptr) {
                samples->push_back({x, y, plane, character, palette, color,
                    pntAddress, rowAddress, cramAddress, upper,
                    static_cast<std::uint16_t>(lower), vdp2VramWord(rowAddress & ~1U),
                    cramAddress == kVdp2CramSize ? std::uint16_t{} : vdp2CramWord(cramAddress),
                    !windowsEnabled || vdp2WindowAllows(layer, x, y)});
            }
            if (color != 0U && (!windowsEnabled || vdp2WindowAllows(layer, x, y))) {
                const auto outputIndex = static_cast<std::size_t>(y) * kFrameWidth + x;
                pixels[outputIndex] = color;
                if (specialPriority != nullptr) {
                    const auto specialLsb = specialPriorityMode == 0U ||
                            patternSpecialPriority == 0U ? 0U
                        : specialPriorityMode == 1U ? 1U
                        : (vdp2SpecialColorCodeMatches(specialPrioritySelect,
                                                       specialPriorityCodes,
                                                       layer, dotColorCode) ? 1U : 0U);
                    (*specialPriority)[outputIndex] = static_cast<std::uint8_t>(specialLsb);
                }
                if (colorCalculationFlags != nullptr && specialMode != 0U) {
                    const auto patternSpecialColorCalculation = oneWord
                        ? (patternControl & 0x0100U) != 0U
                        : (upper & 0x1000U) != 0U;
                    const auto specialEnabled = specialMode == 1U
                        ? patternSpecialColorCalculation
                        : specialMode == 2U
                            ? paletteFormat && patternSpecialColorCalculation &&
                                vdp2SpecialColorCodeMatches(
                                    specialFunctionSelect, specialFunctionCodes,
                                    layer, dotColorCode)
                            : !paletteFormat || (rawColorData & 0x8000U) != 0U;
                    (*colorCalculationFlags)[outputIndex] =
                        specialEnabled ? 1U : 0U;
                }
            }
        }
    }
    if (traceReads) {
        for (std::size_t address = 0x70000U; address < 0x72000U; address += 2U) {
            const auto value = vdp2VramWord(address);
            if (value == 0U) continue;
            ++readRecord.expectedNonZeroWords;
            if (readRecord.expectedFirstNonZeroAddress == kVdp2VramSize) {
                readRecord.expectedFirstNonZeroAddress = address;
                readRecord.expectedFirstNonZeroValue = value;
            }
        }
        mapReadTrace_.push_back(readRecord);
    }
}

bool VideoRuntime::vdp2WindowAllows(unsigned layer, unsigned x,
                                     unsigned y) const noexcept {
    if (layer > 4U) return true;
    const auto control = vdp2Register(layer < 2U ? 0x0D0U :
                                      layer < 4U ? 0x0D2U : 0x0D4U);
    const auto shift = (layer & 1U) == 0U ? 0U : 8U;
    const auto w0Enabled = (control & (1U << (shift + 1U))) != 0U;
    const auto w1Enabled = (control & (1U << (shift + 3U))) != 0U;
    if (!w0Enabled && !w1Enabled) return true;

    const auto inWindow = [this, x, y](unsigned window) {
        const auto xStart = vdp2Register(window == 0U ? 0x0C0U : 0x0C8U) & 0x03FFU;
        const auto yStart = vdp2Register(window == 0U ? 0x0C2U : 0x0CAU) & 0x03FFU;
        const auto xEnd = vdp2Register(window == 0U ? 0x0C4U : 0x0CCU) & 0x03FFU;
        const auto yEnd = vdp2Register(window == 0U ? 0x0C6U : 0x0CEU) & 0x03FFU;
        const auto left = std::min(xStart, xEnd);
        const auto right = std::max(xStart, xEnd);
        const auto top = std::min(yStart, yEnd);
        const auto bottom = std::max(yStart, yEnd);
        return x >= left && x <= right && y >= top && y <= bottom;
    };
    auto w0 = inWindow(0U);
    auto w1 = inWindow(1U);
    // W0A/W1A select the active side: one means draw inside the rectangle,
    // zero means draw outside. Keep the predicate in the same sense as the
    // hardware (and Yabause's TestWindow) instead of inverting the bit.
    if (w0Enabled && (control & (1U << shift)) == 0U) w0 = !w0;
    if (w1Enabled && (control & (1U << (shift + 2U))) == 0U) w1 = !w1;
    if (w0Enabled && w1Enabled) {
        // LOG=0 is AND, LOG=1 is OR for two enabled rectangular windows.
        return (control & (1U << (shift + 6U))) != 0U ? (w0 || w1) : (w0 && w1);
    }
    return w0Enabled ? w0 : w1;
}

void VideoRuntime::setVdp1Renderer(std::shared_ptr<Vdp1ComputeRenderer> renderer) {
    synchronizeVdp1();
    flushVdp1Batch();
    vdp1Renderer_ = std::move(renderer);
    if (vdp1Renderer_) vdp1Batch_.reserve(2000U);
}

bool VideoRuntime::queueVdp1Primitive(Vdp1Primitive primitive) {
    if (!vdp1Renderer_) return false;
    primitive.clip = {vdp1ClipLeft_, vdp1ClipTop_, vdp1ClipRight_, vdp1ClipBottom_};
    vdp1Batch_.push_back(primitive);
    return true;
}

void VideoRuntime::synchronizeVdp1() const {
    if (!vdp1GpuPending_) return;
    vdp1Renderer_->finish(vdp1Framebuffers_[vdp1GpuBank_]);
    vdp1GpuPending_ = false;
}

void VideoRuntime::flushVdp1Batch(bool wait) {
    if (!vdp1Batch_.empty()) {
        synchronizeVdp1();
        vdp1Renderer_->submit(vdp1Vram_, vdp1Framebuffers_[vdp1DrawBank_], vdp1Batch_);
        vdp1GpuBank_ = vdp1DrawBank_;
        vdp1GpuPending_ = true;
        vdp1Batch_.clear();
    }
    if (wait) synchronizeVdp1();
}

void VideoRuntime::executeVdp1Commands() {
    synchronizeVdp1();
    vdp1CommandStats_ = {};
    Vdp1CommandExecution trace;
    if (vdp1CommandTraceEnabled_) {
        trace.drawFrame = frameCount_;
        trace.drawBank = vdp1DrawBank_;
        trace.displayBankAtDraw = vdp1DisplayBank_;
        trace.fbcr = vdp1Register(0x02U);
        trace.ptmr = vdp1Register(0x04U);
        trace.commands.reserve(256U);
    }
    vdp1ClipLeft_ = 0;
    vdp1ClipTop_ = 0;
    vdp1ClipRight_ = static_cast<int>(kFrameWidth) - 1;
    vdp1ClipBottom_ = static_cast<int>(frameHeight()) - 1;
    vdp1LocalX_ = 0;
    vdp1LocalY_ = 0;
    vdp1Edsr_ = static_cast<std::uint16_t>(vdp1Edsr_ & ~2U);
    std::size_t offset = 0;
    std::size_t returnAddress = static_cast<std::size_t>(-1);
    for (std::size_t count = 0; count < 2000U; ++count) {
        if (offset + 0x20U > vdp1Vram_.size()) {
            ++vdp1CommandStats_.malformed;
            break;
        }
        const auto control = vramWord(offset);
        vdp1CommandStats_.lastAddress =
            static_cast<core::Address>(kVdp1Base + offset);
        if (vdp1CommandTraceEnabled_) {
            Vdp1CommandTraceEntry entry;
            entry.address = static_cast<core::Address>(kVdp1Base + offset);
            for (std::size_t word = 0; word < entry.words.size(); ++word) {
                entry.words[word] = vramWord(offset + word * 2U);
            }
            trace.commands.push_back(entry);
        }
        if ((control & 0x8000U) != 0U) break;
        ++vdp1CommandStats_.commands;
        if ((control & 0x4000U) != 0U) {
            ++vdp1CommandStats_.skipped;
        } else {
            drawVdp1Command(offset, control);
        }

        const auto link = static_cast<std::size_t>(vramWord(offset + 2U)) * 8U;
        switch ((control >> 12U) & 0x3U) {
        case 0U: offset += 0x20U; break; // NEXT
        case 1U: offset = link; break; // ASSIGN
        case 2U:
            if (returnAddress == static_cast<std::size_t>(-1)) returnAddress = offset + 0x20U;
            offset = link;
            break; // CALL
        case 3U:
            if (returnAddress != static_cast<std::size_t>(-1)) {
                offset = returnAddress;
                returnAddress = static_cast<std::size_t>(-1);
            } else {
                offset += 0x20U;
            }
            break; // RETURN
        default: break;
        }
    }
    flushVdp1Batch(false);
    if (vdp1CommandStats_.commands >= 2000U) ++vdp1CommandStats_.malformed;
    vdp1Copr_ = static_cast<std::uint16_t>(vdp1CommandStats_.lastAddress >> 3U);
    vdp1Edsr_ = static_cast<std::uint16_t>(vdp1Edsr_ | 2U);
    if (vdp1CommandTraceEnabled_) {
        trace.stats = vdp1CommandStats_;
        vdp1CommandHistory_.push_back(std::move(trace));
        constexpr std::size_t kVdp1CommandHistoryLimit = 8U;
        if (vdp1CommandHistory_.size() > kVdp1CommandHistoryLimit) {
            vdp1CommandHistory_.erase(vdp1CommandHistory_.begin());
        }
    }
}

void VideoRuntime::drawVdp1Command(std::size_t offset, std::uint16_t control) {
    const auto shape = static_cast<unsigned>(control & 0x000FU);
    const auto pmod = vramWord(offset + 0x04U);
    const auto color = vramWord(offset + 0x06U);
    const auto x0 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x0CU))) +
        vdp1LocalX_;
    const auto y0 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x0EU))) +
        vdp1LocalY_;
    const auto x1 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x10U))) +
        vdp1LocalX_;
    const auto y1 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x12U))) +
        vdp1LocalY_;
    const auto x2 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x14U))) +
        vdp1LocalX_;
    const auto y2 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x16U))) +
        vdp1LocalY_;
    const auto x3 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x18U))) +
        vdp1LocalX_;
    const auto y3 = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x1AU))) +
        vdp1LocalY_;
    const auto sourceCell = vramWord(offset + 0x08U);
    const auto size = vramWord(offset + 0x0AU);
    if (control == 0U && pmod == 0U && color == 0U && x0 == 0 && y0 == 0 &&
        x1 == 0 && y1 == 0 && x2 == 0 && y2 == 0 && x3 == 0 && y3 == 0 &&
        sourceCell == 0U && size == 0U) return;
    if (shape == 8U || shape == 9U) {
        // User/system clipping commands update the active plot rectangle.
        // Coordinates are inclusive on VDP1. The clipping rectangle is
        // specified by vertex A (XA,YA) and vertex C (XC,YC); XB/YB and
        // XD/YD are unused for command 9/8. Clamp to the host framebuffer so
        // malformed lists cannot write outside the surface.
        vdp1ClipLeft_ = std::clamp(std::min(x0, x2), 0, static_cast<int>(kFrameWidth) - 1);
        vdp1ClipTop_ = std::clamp(std::min(y0, y2), 0, static_cast<int>(frameHeight()) - 1);
        vdp1ClipRight_ = std::clamp(std::max(x0, x2), 0, static_cast<int>(kFrameWidth) - 1);
        vdp1ClipBottom_ = std::clamp(std::max(y0, y2), 0, static_cast<int>(frameHeight()) - 1);
        return;
    }
    if (shape == 10U) {
        // Local-coordinate commands establish an origin for subsequent
        // primitives. The command's first point is the signed origin.
        vdp1LocalX_ = static_cast<std::int16_t>(vramWord(offset + 0x0CU));
        vdp1LocalY_ = static_cast<std::int16_t>(vramWord(offset + 0x0EU));
        return;
    }
    if (shape <= 2U) {
        ++vdp1CommandStats_.sprites;
        const auto source = static_cast<std::size_t>(sourceCell) * 8U;
        // CMDSIZE stores X in bits 13..8 (in 8-pixel units) and Y in bits
        // 7..0 (pixels). Keep the two fields in their hardware bit positions.
        const auto sourceWidth = static_cast<int>(((size >> 8U) & 0x3FU) * 8U);
        const auto sourceHeight = static_cast<int>(size & 0x00FFU);
        auto left = x0;
        auto top = y0;
        auto width = sourceWidth;
        auto height = sourceHeight;
        auto flipX = (control & 0x0010U) != 0U;
        auto flipY = (control & 0x0020U) != 0U;
        if (shape == 1U && ((control >> 8U) & 0x0FU) == 0U) {
            // With zoom point zero, scaled sprites use A and C as the
            // upper-left and lower-right destination coordinates. B and D
            // are unused. The coordinate order also controls pattern flips.
            left = std::min(x0, x2);
            top = std::min(y0, y2);
            width = std::abs(x2 - x0) + 1;
            height = std::abs(y2 - y0) + 1;
            flipX = flipX != (x2 < x0);
            flipY = flipY != (y2 < y0);
        } else if (shape == 1U) {
            const auto zoom = static_cast<unsigned>((control >> 8U) & 15U);
            const auto horizontal = zoom & 3U;
            const auto vertical = zoom >> 2U;
            if (horizontal == 0U || vertical == 0U) return;
            const auto dx = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x10U)));
            const auto dy = static_cast<int>(static_cast<std::int16_t>(vramWord(offset + 0x12U)));
            const auto firstX = x0 - dx * static_cast<int>(horizontal - 1U) / 2;
            const auto firstY = y0 - dy * static_cast<int>(vertical - 1U) / 2;
            left = std::min(firstX, firstX + dx);
            top = std::min(firstY, firstY + dy);
            width = std::abs(dx) + 1;
            height = std::abs(dy) + 1;
            flipX = flipX != (dx < 0);
            flipY = flipY != (dy < 0);
        } else if (shape == 2U) {
            drawVdp1Quad({x0,y0,x1,y1,x2,y2,x3,y3}, source, sourceWidth,
                sourceHeight, control, pmod, color,
                static_cast<std::size_t>(vramWord(offset + 0x1CU)) * 8U);
            return;
        }
        const auto colorMode = static_cast<unsigned>((pmod >> 3U) & 0x7U);
        if (colorMode <= 5U && sourceWidth > 0 && sourceHeight > 0) {
            width = std::max(1, width);
            height = std::max(1, height);
            const auto minX = std::max({0, -left, vdp1ClipLeft_ - left});
            const auto minY = std::max({0, -top, vdp1ClipTop_ - top});
            const auto maxX = std::min({width, static_cast<int>(kFrameWidth) - left,
                                       vdp1ClipRight_ - left + 1});
            const auto maxY = std::min({height, static_cast<int>(frameHeight()) - top,
                                       vdp1ClipBottom_ - top + 1});
            const auto table = static_cast<std::size_t>(vramWord(offset + 0x1CU)) * 8U;
            const std::array shades{vramWord(table), vramWord(table + 2U),
                                    vramWord(table + 4U), vramWord(table + 6U)};
            Vdp1Primitive primitive{};
            primitive.flags = {Vdp1Primitive::Sprite,
                static_cast<std::uint32_t>((control & ~0x30U) |
                    (flipX ? 0x10U : 0U) | (flipY ? 0x20U : 0U)), pmod, color};
            primitive.points = {left, top, width, height, 0, 0, 0, 0};
            primitive.texture = {static_cast<std::uint32_t>(source),
                static_cast<std::uint32_t>(sourceWidth), static_cast<std::uint32_t>(sourceHeight), 0U};
            primitive.shades = {shades[0], shades[1], shades[2], shades[3]};
            if (queueVdp1Primitive(primitive)) return;
            // Clipping bounds the visible span to kFrameWidth even when x
            // starts far inside a large sprite. Reuse its scaled columns on
            // every row instead of doing integer division for each pixel.
            std::array<int, kFrameWidth> sourceColumns;
            for (int x = minX; x < maxX; ++x) {
                const auto sourceX = x * sourceWidth / width;
                sourceColumns[x - minX] = flipX ? sourceWidth - sourceX - 1 : sourceX;
            }
            for (int y = minY; y < maxY; ++y) {
                const auto sourceY = y * sourceHeight / height;
                const auto sy = flipY ? sourceHeight - sourceY - 1 : sourceY;
                for (int x = minX; x < maxX; ++x) {
                    const auto sx = sourceColumns[x - minX];
                    plotVdp1Pixel(left + x, top + y,
                        sampleVdp1Texture(source, sourceWidth, sourceHeight, sx, sy,
                                          pmod, color), pmod,
                        (pmod & 4U) == 0U ? 0x4210U : interpolateShade(shades,
                            width == 1 ? 0.0 : static_cast<double>(x) / (width - 1),
                            height == 1 ? 0.0 : static_cast<double>(y) / (height - 1)));
                }
            }
        } else {
            fillRectangle(left, top, left + width - 1, top + height - 1, color);
        }
    } else if (shape == 4U) {
        ++vdp1CommandStats_.polygons;
        drawVdp1Quad({x0,y0,x1,y1,x2,y2,x3,y3}, 0U, 0, 0, control, pmod, color,
            static_cast<std::size_t>(vramWord(offset + 0x1CU)) * 8U);
    } else if (shape == 5U) {
        ++vdp1CommandStats_.lines;
        drawLine(x0, y0, x1, y1, color, pmod);
        drawLine(x1, y1, x2, y2, color, pmod);
        drawLine(x2, y2, x3, y3, color, pmod);
        drawLine(x3, y3, x0, y0, color, pmod);
    } else if (shape == 6U) {
        ++vdp1CommandStats_.lines;
        drawLine(x0, y0, x1, y1, color, pmod);
    }
}

void VideoRuntime::drawVdp1Quad(const std::array<int, 8>& points, std::size_t source,
                               int sourceWidth, int sourceHeight, std::uint16_t control,
                               std::uint16_t pmod, std::uint16_t color, std::size_t gouraud) {
    const bool textured = (control & 15U) == 2U;
    if (textured && (sourceWidth <= 0 || sourceHeight <= 0 || ((pmod >> 3U) & 7U) > 5U)) return;
    const auto left = std::max({0, vdp1ClipLeft_, std::min({points[0],points[2],points[4],points[6]})});
    const auto right = std::min({static_cast<int>(kFrameWidth) - 1, vdp1ClipRight_,
                                std::max({points[0],points[2],points[4],points[6]})});
    const auto top = std::max({0, vdp1ClipTop_, std::min({points[1],points[3],points[5],points[7]})});
    const auto bottom = std::min({static_cast<int>(frameHeight()) - 1, vdp1ClipBottom_,
                                 std::max({points[1],points[3],points[5],points[7]})});
    if (left > right || top > bottom) return;
    // P(u,v) = A + (B-A)u + (D-A)v + (A-B+C-D)uv.
    // Reverse this mapping per output pixel. The quad stays continuous across
    // its diagonal; it never becomes two independently interpolated triangles.
    const double bx = points[2] - points[0], by = points[3] - points[1];
    const double cx = points[6] - points[0], cy = points[7] - points[1];
    const double dx = points[0] - points[2] + points[4] - points[6];
    const double dy = points[1] - points[3] + points[5] - points[7];
    const auto cross = [](double ax, double ay, double ex, double ey) { return ax * ey - ay * ex; };
    const double quadratic = -cross(bx, by, dx, dy);
    const std::array shades{vramWord(gouraud), vramWord(gouraud + 2U),
                            vramWord(gouraud + 4U), vramWord(gouraud + 6U)};
    const bool collapsed = cross(bx, by, cx, cy) == 0.0 &&
        cross(bx, by, dx, dy) == 0.0 && cross(cx, cy, dx, dy) == 0.0;
    if (!collapsed) {
        Vdp1Primitive primitive{};
        primitive.flags = {Vdp1Primitive::Quad, control, pmod, color};
        std::copy(points.begin(), points.end(), primitive.points.begin());
        primitive.texture = {static_cast<std::uint32_t>(source),
            static_cast<std::uint32_t>(sourceWidth), static_cast<std::uint32_t>(sourceHeight), 0U};
        primitive.shades = {shades[0], shades[1], shades[2], shades[3]};
        if (queueVdp1Primitive(primitive)) return;
    }
    // Degenerate quads retain the reference point/line path. Complete earlier
    // GPU commands before any CPU plot that reads the destination color.
    flushVdp1Batch();
    const auto plot = [&](int x, int y, double u, double v) {
        auto pixel = color;
        if (textured) {
            auto sx = std::clamp(static_cast<int>(u * sourceWidth), 0, sourceWidth - 1);
            auto sy = std::clamp(static_cast<int>(v * sourceHeight), 0, sourceHeight - 1);
            if ((control & 0x10U) != 0U) sx = sourceWidth - 1 - sx;
            if ((control & 0x20U) != 0U) sy = sourceHeight - 1 - sy;
            pixel = sampleVdp1Texture(source, sourceWidth, sourceHeight, sx, sy, pmod, color);
        }
        plotVdp1Pixel(x, y, pixel, pmod,
            (pmod & 4U) == 0U ? 0x4210U : interpolateShade(shades, u, v));
    };
    if (points[0] == points[2] && points[0] == points[4] && points[0] == points[6] &&
        points[1] == points[3] && points[1] == points[5] && points[1] == points[7]) {
        plot(points[0], points[1], 0.0, 0.0);
        return;
    }
    // A collapsed quad still draws a point/line, with bounded work.
    if (cross(bx, by, cx, cy) == 0.0 && cross(bx, by, dx, dy) == 0.0 &&
        cross(cx, cy, dx, dy) == 0.0) {
        for (unsigned edge = 0U; edge < 4U; ++edge) {
            const auto next = (edge + 1U) & 3U;
            const auto x0 = points[edge * 2U], y0 = points[edge * 2U + 1U];
            const auto x1 = points[next * 2U], y1 = points[next * 2U + 1U];
            const auto steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            if (steps == 0) { plot(x0, y0, 0.0, 0.0); continue; }
            for (int i = 0; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                constexpr std::array<double, 4> us{0.0,1.0,1.0,0.0}, vs{0.0,0.0,1.0,1.0};
                plot(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps,
                     us[edge] + (us[next] - us[edge]) * t,
                     vs[edge] + (vs[next] - vs[edge]) * t);
            }
        }
        return;
    }
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const double px = x - points[0], py = y - points[1];
            const double linear = cross(px, py, dx, dy) - cross(bx, by, cx, cy);
            const double constant = cross(px, py, cx, cy);
            std::array<double, 2> roots{};
            unsigned count = 0U;
            if (std::abs(quadratic) < 1e-9) {
                if (std::abs(linear) > 1e-9) roots[count++] = -constant / linear;
            } else {
                const double discriminant = linear * linear - 4.0 * quadratic * constant;
                if (discriminant >= 0.0) {
                    const double root = std::sqrt(discriminant);
                    roots[count++] = (-linear + root) / (2.0 * quadratic);
                    roots[count++] = (-linear - root) / (2.0 * quadratic);
                }
            }
            for (unsigned index = 0; index < count; ++index) {
                const double u = roots[index];
                if (u < -1e-7 || u > 1.0 + 1e-7) continue;
                const double vx = cx + dx * u, vy = cy + dy * u;
                if (std::max(std::abs(vx), std::abs(vy)) < 1e-9) {
                    if (std::abs(px - bx * u) < 1e-7 && std::abs(py - by * u) < 1e-7) {
                        plot(x, y, std::clamp(u, 0.0, 1.0), 0.0);
                        break;
                    }
                    continue;
                }
                const double v = std::abs(vx) > std::abs(vy)
                    ? (px - bx * u) / vx : (py - by * u) / vy;
                if (v < -1e-7 || v > 1.0 + 1e-7) continue;
                plot(x, y, std::clamp(u, 0.0, 1.0), std::clamp(v, 0.0, 1.0));
                break; // One write per command/pixel, also for translucent quads.
            }
        }
    }
}

void VideoRuntime::plotVdp1Pixel(int x, int y, std::uint16_t color, std::uint16_t pmod,
                                std::uint16_t shade) {
    // SPD disables transparent-pixel rejection. The game uses zero-color
    // polygons with SPD=1 to clear old sprites from the draw framebuffer.
    if ((color == 0U && (pmod & 0x0040U) == 0U) || x < 0 || y < 0 || x >= static_cast<int>(kFrameWidth) ||
        y >= static_cast<int>(frameHeight()) || x < vdp1ClipLeft_ || x > vdp1ClipRight_ ||
        y < vdp1ClipTop_ || y > vdp1ClipBottom_) return;
    if ((pmod & 0x0100U) != 0U && ((x + y) & 1) != 0) return; // mesh
    auto& framebuffer = vdp1Framebuffers_[vdp1DrawBank_];
    const auto offset = (static_cast<std::size_t>(y) * kVdp1FramebufferWidth + x) * 2U;
    const auto destination = static_cast<std::uint16_t>((framebuffer[offset] << 8U) | framebuffer[offset + 1U]);
    if ((color & 0x8000U) != 0U && (pmod & 4U) != 0U) {
        std::uint16_t shaded = 0x8000U;
        for (unsigned shift : {0U, 5U, 10U}) {
            const auto component = static_cast<int>((color >> shift) & 31U) +
                static_cast<int>((shade >> shift) & 31U) - 16;
            shaded |= static_cast<std::uint16_t>(std::clamp(component, 0, 31) << shift);
        }
        color = shaded;
    }
    const auto half = [](std::uint16_t rgb) { return static_cast<std::uint16_t>((rgb >> 1U) & 0x3DEFU); };
    switch (pmod & 3U) {
    case 1U: // shadow operates on an RGB destination
        if ((destination & 0x8000U) == 0U) return;
        color = static_cast<std::uint16_t>(0x8000U | half(destination));
        break;
    case 2U:
        if ((color & 0x8000U) != 0U) color = static_cast<std::uint16_t>(0x8000U | half(color));
        break;
    case 3U:
        if ((color & destination & 0x8000U) != 0U) {
            color = static_cast<std::uint16_t>(0x8000U | (half(color) + half(destination)));
        }
        break;
    default: break;
    }
    if ((pmod & 0x8000U) != 0U) color |= 0x8000U;
    writeFramebufferPixel(framebuffer, x, y, color);
}

void VideoRuntime::fillRectangle(int left, int top, int right, int bottom,
                                 std::uint16_t color) {
    if (color == 0U) return;
    if (left > right) std::swap(left, right);
    if (top > bottom) std::swap(top, bottom);
    if (right < 0 || bottom < 0 || left >= static_cast<int>(kFrameWidth) ||
        top >= static_cast<int>(frameHeight())) return;
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, static_cast<int>(kFrameWidth) - 1);
    bottom = std::min(bottom, static_cast<int>(frameHeight()) - 1);
    Vdp1Primitive primitive{};
    primitive.flags = {Vdp1Primitive::Fill, 0U, 0U, color};
    primitive.points = {left, top, right, bottom, 0, 0, 0, 0};
    if (queueVdp1Primitive(primitive)) return;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            writeFramebufferPixel(vdp1Framebuffers_[vdp1DrawBank_], x, y, color);
        }
    }
}

void VideoRuntime::drawLine(int x0, int y0, int x1, int y1, std::uint16_t color,
                            std::uint16_t pmod) {
    flushVdp1Batch();
    const auto dx = std::abs(x1 - x0);
    const auto sx = x0 < x1 ? 1 : -1;
    const auto dy = -std::abs(y1 - y0);
    const auto sy = y0 < y1 ? 1 : -1;
    auto error = dx + dy;
    while (true) {
        plotVdp1Pixel(x0, y0, color, pmod);
        if (x0 == x1 && y0 == y1) break;
        const auto twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

std::uint16_t VideoRuntime::sampleVdp1Texture(std::size_t source, int width, int height,
                                               int x, int y, std::uint16_t pmod,
                                               std::uint16_t color) const noexcept {
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height) return 0U;
    const auto mode = static_cast<unsigned>((pmod >> 3U) & 0x7U);
    const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
        static_cast<std::size_t>(x);
    const auto transparentZero = (pmod & 0x0040U) == 0U;
    const auto bankColor = [color](unsigned index, unsigned indexBits) {
        const auto indexMask = (1U << indexBits) - 1U;
        // Preserve the 16-bit color-bank code in the framebuffer. VDP2 applies
        // CRAOFB and resolves the CRAM entry during final composition.
        return static_cast<std::uint16_t>((static_cast<unsigned>(color) &
                                           (0x7FFFU & ~indexMask)) |
                                          (index & indexMask));
    };
    if (mode <= 1U) {
        const auto packed = vramWord(source + (pixel / 4U) * 2U);
        const auto shift = static_cast<unsigned>(3U - (pixel & 3U)) * 4U;
        const auto index = static_cast<unsigned>((packed >> shift) & 0x0FU);
        if (mode == 0U) return transparentZero && index == 0U ? 0U : bankColor(index, 4U);
        // Lookup modes contain sixteen 16-bit framebuffer color codes. VDP2
        // decides whether each entry is RGB555 or a CRAM code.
        const auto lookup = vramWord(static_cast<std::size_t>(color) * 8U + index * 2U);
        if ((transparentZero && index == 0U) || lookup == 0U) return 0U;
        return lookup;
    } else if (mode <= 4U) {
        const auto packed = vramWord(source + (pixel / 2U) * 2U);
        const auto index = static_cast<unsigned>(
            (pixel & 1U) == 0U ? (packed >> 8U) & 0xFFU : packed & 0xFFU);
        const auto indexBits = mode == 2U ? 6U : mode == 3U ? 7U : 8U;
        return transparentZero && index == 0U ? 0U : bankColor(index, indexBits);
    } else {
        // Mode 5 is the 16-bit RGB mode. Zero is the transparent code; the
        // remaining RGB codes are copied to the framebuffer unchanged.
        return vramWord(source + pixel * 2U);
    }
}

void VideoRuntime::writeFramebufferPixel(std::vector<std::uint8_t>& framebuffer,
                                         int x, int y, std::uint16_t color) const noexcept {
    // Raw framebuffer write. Transparent rejection belongs to the command's
    // SPD handling in plotVdp1Pixel; accepted zero pixels must erase old data.
    if (x < 0 || y < 0 || x >= static_cast<int>(kFrameWidth) ||
        y >= static_cast<int>(frameHeight()) || x < vdp1ClipLeft_ || x > vdp1ClipRight_ ||
        y < vdp1ClipTop_ || y > vdp1ClipBottom_) return;
    // VDP1 stores the visible area in 512-pixel framebuffer rows. The host
    // presents the TVMD-selected viewport, while guest memory keeps the
    // hardware pitch.
    const auto offset = (static_cast<std::size_t>(y) * kVdp1FramebufferWidth +
                        static_cast<std::size_t>(x)) * 2U;
    if (offset + 1U >= framebuffer.size()) return;
    framebuffer[offset] = static_cast<std::uint8_t>(color >> 8U);
    framebuffer[offset + 1U] = static_cast<std::uint8_t>(color);
}

} // namespace saturn::platform
