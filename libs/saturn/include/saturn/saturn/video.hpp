#pragma once

#include "saturn/core/address.hpp"
#include "saturn/saturn/vdp1_compute.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace saturn::platform {

struct VideoFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t sequence{};
    std::vector<std::uint32_t> pixels;
};

using VideoFrameSink = std::function<void(const VideoFrame&)>;

struct VideoPixelBounds {
    std::size_t nonZeroPixels{};
    std::uint32_t minX{};
    std::uint32_t minY{};
    std::uint32_t maxX{};
    std::uint32_t maxY{};
};

struct VideoMemoryBounds {
    std::size_t nonZeroBytes{};
    std::size_t firstByte{};
    std::size_t lastByte{};
};

// Addresses are the renderer's unmasked VRAM offsets, not a hardware oracle.
struct VideoCellSample {
    std::uint32_t x{}, y{}, plane{}, character{}, palette{}, color{};
    std::size_t mapAddress{}, characterAddress{}, cramAddress{};
    std::uint16_t nameUpper{}, nameLower{}, rawCharacter{}, rawCram{};
    bool windowAllows{};
};

struct VideoWriteRegion {
    std::uint64_t writes{}, lastFrame{};
};

// One aggregate row per NBG0 composition while map-read tracing is enabled.
// renderer* describes the pattern-name words actually consumed by the
// renderer; expected* samples the independently validated NBG0 map range.
struct VideoMapReadRecord {
    std::uint64_t frame{};
    std::size_t contextStep{};
    core::Address contextBlock{};
    core::Address contextCaller{};
    std::size_t rendererReads{};
    std::size_t rendererNonZeroReads{};
    std::size_t rendererMinAddress{};
    std::size_t rendererMaxAddress{};
    std::size_t rendererFirstNonZeroAddress{};
    std::uint16_t rendererFirstNonZeroValue{};
    std::size_t expectedWords{};
    std::size_t expectedNonZeroWords{};
    std::size_t expectedFirstNonZeroAddress{};
    std::uint16_t expectedFirstNonZeroValue{};
};

struct VideoMapStateRecord {
    std::uint64_t frame{};
    std::size_t contextStep{};
    core::Address contextBlock{};
    core::Address contextCaller{};
    std::uint16_t tvmd{};
    std::uint16_t bgon{};
    std::uint16_t chctla{};
    std::uint16_t mpofn{};
    std::uint16_t mapAB{};
    bool nbg0Enabled{};
    bool cellMode{};
    std::size_t expectedWords{};
    std::size_t expectedNonZeroWords{};
    std::size_t expectedFirstNonZeroAddress{};
    std::uint16_t expectedFirstNonZeroValue{};
};

struct Vdp1CommandStats {
    std::size_t commands{};
    std::size_t sprites{};
    std::size_t polygons{};
    std::size_t lines{};
    std::size_t skipped{};
    std::size_t malformed{};
    core::Address lastAddress{};
};

struct Vdp1CommandTraceEntry {
    core::Address address{};
    std::array<std::uint16_t, 16> words{};
};

// Actual VDP1 traversal order, including the terminating command. The
// presentation fields tie the command list to the framebuffer bank that was
// made visible by a later manual FBCR swap.
struct Vdp1CommandExecution {
    std::uint64_t drawFrame{};
    std::uint64_t presentedFrame{};
    bool wasPresented{};
    std::uint8_t drawBank{};
    std::uint8_t displayBankAtDraw{};
    std::uint8_t displayedBank{0xFFU};
    std::uint16_t fbcr{};
    std::uint16_t ptmr{};
    Vdp1CommandStats stats;
    std::vector<Vdp1CommandTraceEntry> commands;
};

// Guest writes to the VDP1 player texture page and control registers.
// Register offsets start at 0x100000 relative to VDP1 VRAM. Context fields
// identify the SH-2 block that populated each byte range.
struct Vdp1MemoryWriteTraceEntry {
    std::size_t step{};
    core::Address blockAddress{};
    core::Address callerBlock{};
    bool registersAvailable{};
    std::array<std::uint32_t, 8> registers{};
    std::uint32_t offset{};
    std::uint8_t width{};
    std::uint32_t value{};
};

// Owned copy of existing state; composed can precede the current VDP1 phase.
// Empty memory vectors mean captureDebugSnapshot(false) omitted those dumps.
struct VideoDebugSnapshot {
    VideoFrame composed;
    std::array<std::vector<std::uint8_t>, 2> framebufferBanks;
    std::vector<std::uint8_t> vdp1Vram, vdp2Vram, cram;
    std::vector<std::uint8_t> vdp1Registers, vdp2Registers;
    std::vector<Vdp1MemoryWriteTraceEntry> vdp1RegisterWriteHistory;
    std::vector<Vdp1MemoryWriteTraceEntry> vdp1MemoryWriteHistory;
    std::vector<Vdp1CommandExecution> vdp1CommandHistory;
    std::size_t contextStep{};
    core::Address contextBlock{}, contextCaller{};
    std::uint64_t frameCount{}, compositionCount{};
    std::uint16_t vdp1Tvmr{}, vdp1Fbcr{}, vdp1Ptmr{};
    std::uint16_t vdp1Ewdr{}, vdp1Ewlr{}, vdp1Ewrr{};
    std::uint16_t vdp1Edsr{}, vdp1Lopr{}, vdp1Copr{};
    std::uint8_t vdp1DisplayBank{}, vdp1DrawBank{};
    bool vdp1SwapPending{}, vdp1ErasePending{}, vdp1VblankErasePending{};
    bool vdp1InVBlank{}, vdp1DrawEndPending{}, vdp1FramebufferConfigured{};
    bool compositionDirty{}, framePending{};
};

// Snapshot emitted at a scheduler presentation boundary. Both VDP1 banks are
// exposed so manual FBCR switching can be inspected independently of the
// final composite.
struct VideoDiagnosticSnapshot {
    std::vector<std::uint8_t> vramDump, cramDump, registerDump, vdp1VramDump;
    // One entry per 32 bytes; writes count bus writes touching that region.
    std::vector<VideoWriteRegion> vramWrites, cramWrites;
    std::vector<VideoCellSample> cellSamples;
    std::vector<Vdp1CommandExecution> vdp1CommandHistory;
    std::vector<Vdp1MemoryWriteTraceEntry> vdp1MemoryWriteHistory;
    bool priorityConfigured{};
    VideoFrame vdp2;
    VideoFrame back;
    VideoFrame nbg0;
    std::array<VideoFrame, 4> vdp2Layers;
    std::array<VideoFrame, 2> vdp1Banks;
    VideoFrame composed;
    std::array<VideoPixelBounds, 2> vdp1Bounds;
    VideoPixelBounds vdp2Bounds;
    VideoPixelBounds backBounds;
    VideoPixelBounds nbg0Bounds;
    std::array<VideoPixelBounds, 4> vdp2LayerBounds;
    VideoPixelBounds composedBounds;
    VideoMemoryBounds vdp1Memory;
    VideoMemoryBounds vdp2VramMemory;
    VideoMemoryBounds vdp2CramMemory;

    std::uint16_t vdp1Tvmr{};
    std::uint16_t vdp1Fbcr{};
    std::uint16_t vdp1Ptmr{};
    std::uint16_t vdp1Ewdr{};
    std::uint16_t vdp1Ewlr{};
    std::uint16_t vdp1Ewrr{};
    std::uint16_t vdp1Edsr{};
    std::uint16_t vdp1Lopr{};
    std::uint16_t vdp1Copr{};
    std::uint8_t vdp1DisplayBank{};
    std::uint8_t vdp1DrawBank{};
    bool vdp1SwapPending{};

    std::uint16_t vdp2Tvmd{};
    std::uint16_t vdp2Bgon{};
    std::uint16_t vdp2Chctla{};
    std::uint16_t vdp2Chctlb{};
    std::uint16_t vdp2Bmpna{};
    std::uint16_t vdp2Bmpnb{};
    std::uint16_t vdp2Mpofn{};
    std::uint16_t vdp2Mpofr{};
    std::uint16_t vdp2Prina{};
    std::uint16_t vdp2Prinb{};
    std::uint16_t vdp2Prir{};
    std::uint16_t vdp2Spctl{};
    std::uint16_t vdp2Ccctl{};
    std::uint16_t vdp2Wctla{};
    std::uint16_t vdp2Wpsx0{};
    std::uint16_t vdp2Wpsy0{};
    std::uint16_t vdp2Wpex0{};
    std::uint16_t vdp2Wpey0{};
    // NBG0 line/vertical-cell scroll state.  Rockman X4 uses these tables
    // to make the 320x224 RGB888 bitmap contiguous in VRAM even though
    // CHCTLA advertises the hardware's 512x256 bitmap aperture.
    std::uint16_t vdp2Scrctl{};
    std::uint16_t vdp2Vcstau{};
    std::uint16_t vdp2Vcstal{};
    std::uint16_t vdp2Lsta0u{};
    std::uint16_t vdp2Lsta0l{};
    std::vector<std::uint16_t> vdp2LineScrollWords;
    std::vector<std::uint16_t> vdp2VerticalCellScrollWords;
    std::size_t vdp2LineScrollNonZeroWords{};
    std::size_t vdp2VerticalCellScrollNonZeroWords{};
    std::size_t vdp2LineScrollFirstNonZero{};
    std::size_t vdp2VerticalCellScrollFirstNonZero{};
    std::uint32_t vdp2BackAddress{};
    std::uint16_t vdp2BackWord{};
    bool vdp2BackLineMode{};
    std::vector<std::uint16_t> vdp2BackLineWords;
    std::uint32_t vdp2Nbg0Base{};
    std::uint32_t vdp2Nbg0Width{};
    std::uint32_t vdp2Nbg0Height{};
    std::uint32_t vdp2Nbg0BitsPerPixel{};
    std::uint32_t vdp2Nbg0RowPitch{};
    std::vector<std::uint32_t> vdp2Nbg0LineFirstRaw;
    // FNV-1a over the raw VRAM words sampled in display order for each line.
    // This is compared with the decoder-boundary row hash to detect any
    // transformation between DMA publication and VDP2 sampling.
    std::vector<std::uint32_t> vdp2Nbg0LineRawHash;
    std::vector<std::size_t> vdp2Nbg0LineRawNonZero;
    std::vector<std::size_t> vdp2Nbg0LineAlphaNonZero;
    std::vector<std::size_t> vdp2Nbg0LineNonZero;
};

using VideoDiagnosticSink = std::function<void(const VideoDiagnosticSnapshot&)>;

// Minimal VDP1/VDP2 runtime. It preserves the Saturn memory layout and
// register side effects needed by boot/game code. PTMR=1 walks the VDP1
// command table immediately; PTMR=2 repeats the walk at each presentation
// boundary. The supported primitive subset is rendered into a deterministic
// RGB555 frame.
class VideoRuntime {
public:
    VideoRuntime() = default;
    VideoRuntime(const VideoRuntime&) = delete;
    VideoRuntime& operator=(const VideoRuntime&) = delete;
    void setVdp1Renderer(std::shared_ptr<Vdp1ComputeRenderer> renderer);
    // Live ports compose at scan-out. Reference/diagnostic mode keeps immediate
    // host frames after each manual VDP1 draw for existing observers.
    void setDeferredComposition(bool enabled);
    [[nodiscard]] std::uint64_t compositionCount() const noexcept { return compositionCount_; }
    static constexpr core::Address kVdp1Base = 0x05C00000U;
    static constexpr core::Address kVdp1Framebuffer = kVdp1Base + 0x00080000U;
    static constexpr core::Address kVdp1Registers = kVdp1Base + 0x00100000U;
    static constexpr core::Address kVdp2Base = 0x05E00000U;
    static constexpr core::Address kVdp2Cram = kVdp2Base + 0x00100000U;
    static constexpr core::Address kVdp2Registers = kVdp2Base + 0x00180000U;

    static constexpr std::uint32_t kVdp2Tvmd = kVdp2Registers + 0x000U;
    static constexpr std::uint32_t kVdp2Bgon = kVdp2Registers + 0x020U;
    static constexpr std::uint32_t kVdp2Chctla = kVdp2Registers + 0x028U;
    static constexpr std::uint32_t kVdp2Chctlb = kVdp2Registers + 0x02AU;
    static constexpr std::uint32_t kVdp2Bmpna = kVdp2Registers + 0x02CU;
    static constexpr std::uint32_t kVdp2Pncn0 = kVdp2Registers + 0x030U;
    static constexpr std::uint32_t kVdp2Pncn1 = kVdp2Registers + 0x032U;
    static constexpr std::uint32_t kVdp2Pncn2 = kVdp2Registers + 0x034U;
    static constexpr std::uint32_t kVdp2Pncn3 = kVdp2Registers + 0x036U;
    static constexpr std::uint32_t kVdp2Mpofn = kVdp2Registers + 0x03CU;
    static constexpr std::uint32_t kVdp2Plsz = kVdp2Registers + 0x03AU;
    static constexpr std::uint32_t kVdp2Mpabn0 = kVdp2Registers + 0x040U;
    static constexpr std::uint32_t kVdp2Mpcdn0 = kVdp2Registers + 0x042U;
    static constexpr std::uint32_t kVdp2Mpabn1 = kVdp2Registers + 0x044U;
    static constexpr std::uint32_t kVdp2Mpcdn1 = kVdp2Registers + 0x046U;
    static constexpr std::uint32_t kVdp2Mpabn2 = kVdp2Registers + 0x048U;
    static constexpr std::uint32_t kVdp2Mpcdn2 = kVdp2Registers + 0x04AU;
    static constexpr std::uint32_t kVdp2Mpabn3 = kVdp2Registers + 0x04CU;
    static constexpr std::uint32_t kVdp2Mpcdn3 = kVdp2Registers + 0x04EU;
    static constexpr std::uint32_t kVdp2Scxin0 = kVdp2Registers + 0x070U;
    static constexpr std::uint32_t kVdp2Scyin0 = kVdp2Registers + 0x074U;
    static constexpr std::uint32_t kVdp2Scxin1 = kVdp2Registers + 0x080U;
    static constexpr std::uint32_t kVdp2Scyin1 = kVdp2Registers + 0x084U;
    static constexpr std::uint32_t kVdp2Scxin2 = kVdp2Registers + 0x088U;
    static constexpr std::uint32_t kVdp2Scyin2 = kVdp2Registers + 0x08CU;
    static constexpr std::uint32_t kVdp2Scxin3 = kVdp2Registers + 0x090U;
    static constexpr std::uint32_t kVdp2Scyin3 = kVdp2Registers + 0x094U;
    static constexpr std::uint32_t kVdp2Scrctl = kVdp2Registers + 0x09AU;
    static constexpr std::uint32_t kVdp2Vcstau = kVdp2Registers + 0x09CU;
    static constexpr std::uint32_t kVdp2Vcstal = kVdp2Registers + 0x09EU;
    static constexpr std::uint32_t kVdp2Lsta0u = kVdp2Registers + 0x0A0U;
    static constexpr std::uint32_t kVdp2Lsta0l = kVdp2Registers + 0x0A2U;
    static constexpr std::uint32_t kVdp2Bktau = kVdp2Registers + 0x0ACU;
    static constexpr std::uint32_t kVdp2Bktal = kVdp2Registers + 0x0AEU;
    static constexpr std::uint32_t kVdp2Craofb = kVdp2Registers + 0x0E6U;
    static constexpr std::uint32_t kVdp2Wpsx0 = kVdp2Registers + 0x0C0U;
    static constexpr std::uint32_t kVdp2Wpsy0 = kVdp2Registers + 0x0C2U;
    static constexpr std::uint32_t kVdp2Wpex0 = kVdp2Registers + 0x0C4U;
    static constexpr std::uint32_t kVdp2Wpey0 = kVdp2Registers + 0x0C6U;
    static constexpr std::uint32_t kVdp2Wpsx1 = kVdp2Registers + 0x0C8U;
    static constexpr std::uint32_t kVdp2Wpsy1 = kVdp2Registers + 0x0CAU;
    static constexpr std::uint32_t kVdp2Wpex1 = kVdp2Registers + 0x0CCU;
    static constexpr std::uint32_t kVdp2Wpey1 = kVdp2Registers + 0x0CEU;
    static constexpr std::uint32_t kVdp2Wctla = kVdp2Registers + 0x0D0U;
    static constexpr std::uint32_t kVdp2Wctlb = kVdp2Registers + 0x0D2U;
    static constexpr std::uint16_t kVdp2DisplayEnable = 0x8000U;
    static constexpr std::uint16_t kVdp2Nbg0Enable = 0x0001U;
    static constexpr std::uint16_t kVdp2Nbg1Enable = 0x0002U;
    static constexpr std::uint16_t kVdp2Nbg2Enable = 0x0004U;
    static constexpr std::uint16_t kVdp2Nbg3Enable = 0x0008U;

    static constexpr std::size_t kVdp1VramSize = 0x80000U;
    static constexpr std::size_t kVdp1FramebufferSize = 0x40000U;
    static constexpr std::uint32_t kVdp1FramebufferWidth = 512U;
    static constexpr std::uint32_t kVdp1FramebufferHeight = 256U;
    static constexpr std::size_t kVdp2VramSize = 0x80000U;
    static constexpr std::size_t kVdp2CramSize = 0x1000U;
    static constexpr std::size_t kRegisterSize = 0x200U;
    static constexpr std::uint32_t kFrameWidth = 320U;
    static constexpr std::uint32_t kFrameHeight = 224U;

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) const;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                             std::uint32_t value);

    void advanceFrame(bool startAutomaticDraw = true);
    [[nodiscard]] std::uint64_t frameCount() const noexcept;
    [[nodiscard]] bool framePending() const noexcept;
    [[nodiscard]] const VideoFrame& lastFrame() const noexcept;
    // Human-readable description of what wins at one screen pixel: the VDP2
    // layer priority chain (after the special priority function) and the VDP1
    // sprite colour/priority decoded from the displayed framebuffer. Used by
    // --vdp2-probe to settle occlusion cases with data instead of guesses.
    [[nodiscard]] std::string describeVdp2Pixel(unsigned x, unsigned y) const;
    [[nodiscard]] bool consumeFrame(VideoFrame& output);
    // Present the latest composed frame to the host. Drawing and composition
    // may happen several times between VBlank edges; presentation is paced by
    // the scheduler and therefore must not be tied to every PTMR=1 draw.
    void presentFrame();
    // Scheduler phases: the guest may request VBE between IN and OUT. Finish
    // erase/change before the OUT handler clears VBE, then start PTMR=2 on
    // the new draw bank and publish one host frame.
    void onVBlankIn();
    void onVBlankOut();
    // Called synchronously on the emulation thread. The sink may capture a
    // snapshot, but must not mutate/re-enter the video runtime or perform I/O.
    void setDebugEventSink(std::function<void(std::string_view)> sink);
    [[nodiscard]] VideoDebugSnapshot captureDebugSnapshot(bool includeMemory) const;
    void setFrameSink(VideoFrameSink sink);
    void setDiagnosticSink(VideoDiagnosticSink sink);
    void setDiagnosticCaptureEnabled(bool enabled) noexcept;
    void setDiagnosticMemoryCapture(bool enabled) noexcept;
    void setVdp1CommandTraceEnabled(bool enabled) noexcept;
    void setVdp1MemoryWriteTraceEnabled(bool enabled) noexcept;
    [[nodiscard]] bool vdp1MemoryWriteTraceEnabled() const noexcept;
    void setVdp1MemoryWriteRegisters(
        const std::array<std::uint32_t, 8>& registers,
        bool available) noexcept;
    void setExecutionContext(std::size_t step, core::Address block,
                             core::Address caller) noexcept;
    void setMapReadTrace(bool enabled);
    [[nodiscard]] const std::vector<VideoMapReadRecord>& mapReadTrace() const noexcept;
    [[nodiscard]] const std::vector<VideoMapStateRecord>& mapStateTrace() const noexcept;
    // Capture once after visible output, display-off, then TVMD=8010 black.
    void captureFirstReenabledBlackFrame(VideoDiagnosticSink sink);
    [[nodiscard]] bool displayEnabled() const noexcept;
    [[nodiscard]] const Vdp1CommandStats& vdp1CommandStats() const noexcept;

    // PTMR=1 starts a command-list execution immediately. PTMR=2 starts it at
    // each frame boundary. The draw-end edge is consumed by ScuEventScheduler
    // at the next non-preemptive block boundary.
    [[nodiscard]] bool takeVdp1DrawEnd() noexcept;

private:
    [[nodiscard]] bool changeVdp1Framebuffer(std::uint64_t presentedFrame);
    void presentCurrentFrame();
    struct Vdp2CompositePixel {
        std::uint32_t topColor{};
        std::uint32_t secondColor{};
        std::uint32_t thirdColor{};
        std::uint8_t topPriority{};
        std::uint8_t secondPriority{};
        std::uint8_t topLayer{};
        std::uint8_t secondLayer{};
        std::uint8_t topRatio{};
        std::uint8_t secondRatio{};
        std::uint8_t flags{};
    };

    [[nodiscard]] static std::uint32_t readBytes(const std::vector<std::uint8_t>& memory,
                                                  std::size_t offset,
                                                  std::uint8_t width) noexcept;
    static void writeBytes(std::vector<std::uint8_t>& memory, std::size_t offset,
                           std::uint8_t width, std::uint32_t value) noexcept;
    [[nodiscard]] std::uint16_t vdp1Register(std::uint32_t offset) const noexcept;
    [[nodiscard]] std::uint16_t vdp2Register(std::uint32_t offset) const noexcept;
    [[nodiscard]] std::uint32_t frameHeight() const noexcept;
    void writeVdp1Register(std::uint32_t offset, std::uint16_t value);
    void writeVdp2Register(std::uint32_t offset, std::uint16_t value);
    void startVdp1Draw();
    void eraseVdp1DisplayFramebuffer();
    void composeFrame();
    void executeVdp1Commands();
    bool queueVdp1Primitive(Vdp1Primitive primitive);
    void flushVdp1Batch(bool wait = true);
    void synchronizeVdp1() const;
    void composeVdp2(std::vector<std::uint32_t>& pixels,
                     std::array<VideoFrame, 4>* layerFrames = nullptr,
                     bool resolveOutput = true);
    void composeVdp2Back(std::vector<std::uint32_t>& pixels);
    void composeVdp2RotationBitmap(std::vector<std::uint32_t>& pixels,
                                   std::vector<std::uint8_t>* colorCalculationFlags);
    void composeVdp1Overlay(std::vector<std::uint32_t>& pixels);
    [[nodiscard]] std::uint32_t decodeVdp1FramebufferPixel(
        std::uint16_t value, std::uint8_t& priority,
        std::uint8_t& colorCalculationRatioIndex) const noexcept;
    [[nodiscard]] std::uint8_t colorCalculationRatioForLayer(
        std::uint8_t layer) const noexcept;
    [[nodiscard]] std::uint32_t resolveColorCalculation(
        const Vdp2CompositePixel& pixel) const noexcept;
    void composeVdp2Bitmap(std::vector<std::uint32_t>& pixels, unsigned layer,
                           std::uint16_t characterControl,
                           std::uint16_t bitmapPalette,
                           std::uint16_t mapOffset,
                           std::uint16_t scrollX,
                           std::uint16_t scrollY,
                           std::vector<std::uint8_t>* colorCalculationFlags = nullptr);
    void composeVdp2Cell(std::vector<std::uint32_t>& pixels, unsigned layer,
                         std::uint16_t characterControl, std::uint16_t patternControl,
                         std::uint16_t mapOffset, std::uint16_t mapA,
                         std::uint16_t mapC, std::uint16_t scrollX,
                         std::uint16_t scrollY,
                         std::vector<VideoCellSample>* samples = nullptr,
                         std::vector<std::uint8_t>* colorCalculationFlags = nullptr,
                         std::vector<std::uint8_t>* specialPriority = nullptr);
    [[nodiscard]] bool vdp2WindowAllows(unsigned layer, unsigned x,
                                         unsigned y) const noexcept;
    void drawVdp1Command(std::size_t offset, std::uint16_t control);
    void drawVdp1Quad(const std::array<int, 8>& vertices, std::size_t source,
                     int sourceWidth, int sourceHeight, std::uint16_t control,
                     std::uint16_t pmod, std::uint16_t color, std::size_t gouraud);
    void plotVdp1Pixel(int x, int y, std::uint16_t color, std::uint16_t pmod,
                      std::uint16_t shade = 0x4210U);
    void fillRectangle(int left, int top, int right, int bottom, std::uint16_t color);
    void drawLine(int x0, int y0, int x1, int y1, std::uint16_t color,
                  std::uint16_t pmod = 0U);
    [[nodiscard]] std::uint16_t vramWord(std::size_t offset) const noexcept;
    [[nodiscard]] std::uint16_t vdp2VramWord(std::size_t offset) const noexcept;
    [[nodiscard]] std::uint16_t vdp2CramWord(std::size_t offset) const noexcept;
    [[nodiscard]] VideoFrame makeVdp1DiagnosticFrame(unsigned bank) const;
    [[nodiscard]] static VideoPixelBounds pixelBounds(const VideoFrame& frame) noexcept;
    [[nodiscard]] static VideoMemoryBounds memoryBounds(
        const std::vector<std::uint8_t>& memory) noexcept;
    [[nodiscard]] std::uint16_t sampleVdp1Texture(std::size_t source,
                                                   int width, int height,
                                                   int x, int y,
                                                   std::uint16_t pmod,
                                                   std::uint16_t color) const noexcept;
    void writeFramebufferPixel(std::vector<std::uint8_t>& framebuffer,
                               int x, int y, std::uint16_t color) const noexcept;

    std::vector<std::uint8_t> vdp1Vram_ = std::vector<std::uint8_t>(kVdp1VramSize);
    mutable std::array<std::vector<std::uint8_t>, 2> vdp1Framebuffers_{
        std::vector<std::uint8_t>(kVdp1FramebufferSize),
        std::vector<std::uint8_t>(kVdp1FramebufferSize)};
    std::vector<std::uint8_t> vdp1Registers_ = std::vector<std::uint8_t>(kRegisterSize);
    std::vector<std::uint8_t> vdp2Vram_ = std::vector<std::uint8_t>(kVdp2VramSize);
    std::vector<std::uint8_t> vdp2Cram_ = std::vector<std::uint8_t>(kVdp2CramSize);
    std::vector<std::uint8_t> vdp2Registers_ = std::vector<std::uint8_t>(kRegisterSize);
    std::uint16_t vdp1Edsr_{};
    std::uint16_t vdp1Lopr_{};
    std::uint16_t vdp1Copr_{};
    int vdp1ClipLeft_{};
    int vdp1ClipTop_{};
    int vdp1ClipRight_{static_cast<int>(kFrameWidth) - 1};
    int vdp1ClipBottom_{static_cast<int>(kFrameHeight) - 1};
    int vdp1LocalX_{};
    int vdp1LocalY_{};
    std::uint16_t vdp2Tvstat_{};
    bool vdp2PriorityConfigured_{};
    std::vector<Vdp2CompositePixel> vdp2CompositePixels_;
    // Layers are rendered/merged sequentially; retain scratch storage between frames.
    std::vector<std::uint32_t> vdp2LayerScratch_;
    std::vector<std::uint8_t> vdp2CalculationScratch_;
    // Per-dot least significant bit of the priority number from the VDP2
    // special priority function (SFPRMD), one byte per output pixel.
    std::vector<std::uint8_t> vdp2SpecialScratch_;
    Vdp1CommandStats vdp1CommandStats_{};
    std::shared_ptr<Vdp1ComputeRenderer> vdp1Renderer_;
    std::vector<Vdp1Primitive> vdp1Batch_;
    mutable bool vdp1GpuPending_{};
    std::uint8_t vdp1GpuBank_{};
    bool vdp1DrawEndPending_{};
    std::uint8_t vdp1DisplayBank_{};
    // Before FBCR is configured the VDP1 exposes one logical
    // framebuffer. Keep both selectors on bank zero so existing direct VRAM
    // tests and software that writes before configuring FBCR remain visible.
    std::uint8_t vdp1DrawBank_{};
    bool vdp1FramebufferConfigured_{};
    bool vdp1SwapPending_{};
    bool vdp1ErasePending_{};
    bool vdp1InVBlank_{}, vdp1VblankErasePending_{};
    std::uint64_t frameCount_{};
    bool framePending_{};
    bool deferredComposition_{}, compositionDirty_{};
    std::uint64_t compositionCount_{};
    VideoFrame lastFrame_{kFrameWidth, kFrameHeight, 0U,
                          std::vector<std::uint32_t>(kFrameWidth * kFrameHeight)};
    VideoFrameSink frameSink_;
    VideoDiagnosticSink diagnosticSink_;
    std::function<void(std::string_view)> debugEventSink_;
    bool diagnosticCaptureEnabled_{true};
    bool blackProbe_{}, probeSawVisible_{}, probeSawOff_{}, probeCaptured_{};
    bool diagnosticMemoryCapture_{};
    bool vdp1CommandTraceEnabled_{};
    std::vector<Vdp1CommandExecution> vdp1CommandHistory_;
    bool vdp1MemoryWriteTraceEnabled_{};
    std::deque<Vdp1MemoryWriteTraceEntry> vdp1MemoryWriteHistory_;
    std::deque<Vdp1MemoryWriteTraceEntry> vdp1RegisterWriteHistory_;
    std::array<std::uint32_t, 8> vdp1MemoryWriteRegisters_{};
    bool vdp1MemoryWriteRegistersAvailable_{};
    std::vector<VideoWriteRegion> vramWrites_, cramWrites_;
    bool mapReadTraceEnabled_{};
    std::size_t contextStep_{};
    core::Address contextBlock_{};
    core::Address contextCaller_{};
    std::vector<VideoMapReadRecord> mapReadTrace_;
    std::vector<VideoMapStateRecord> mapStateTrace_;
};

} // namespace saturn::platform
