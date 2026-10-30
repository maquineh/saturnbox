#pragma once

#include "saturn/core/address.hpp"
#include "saturn/saturn/mmio.hpp"
#include "saturn/sir/interpreter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace saturn::platform {

// Deterministic SCU event source for the single-threaded runtime. A block is
// the scheduler's unit of time; this keeps tests reproducible while still
// giving the game the same pending/masked interrupt observations as hardware.
class ScuEventScheduler final {
public:
    static constexpr std::uint32_t kVBlankInStatus = 0x00000001U;
    static constexpr std::uint8_t kVBlankInVector = 0x40U;
    static constexpr std::uint32_t kVBlankOutStatus = 0x00000002U;
    static constexpr std::uint8_t kVBlankOutVector = 0x41U;
    static constexpr std::uint32_t kHBlankInStatus = 0x00000004U;
    static constexpr std::uint8_t kHBlankInVector = 0x42U;
    static constexpr std::uint32_t kTimer0Status = 0x00000008U;
    static constexpr std::uint8_t kTimer0Vector = 0x43U;
    static constexpr std::size_t kScanlinesPerFrame = 263U;
    static constexpr std::size_t kVBlankScanlines = 40U;
    static constexpr std::uint64_t kProfileSampleInterval = 1009U;
    // VDP1 command-list end (sprite draw end), SCU factor bit 13 / vector 0x4D.
    static constexpr std::uint32_t kSpriteDrawEndStatus = 0x00002000U;
    static constexpr std::uint8_t kSpriteDrawEndVector = 0x4DU;
    // SCU interrupt status bits for DMA level 2/1/0 completion.  The three
    // DMA channels occupy bits 9..11 and vectors 0x49..0x4B respectively.
    static constexpr std::uint32_t kLevel2DmaEndStatus = 0x00000200U;
    static constexpr std::uint32_t kLevel1DmaEndStatus = 0x00000400U;
    static constexpr std::uint32_t kLevel0DmaEndStatus = 0x00000800U;
    static constexpr core::Address kScuInterruptTable = 0x06000900U;
    // The Rockman runtime keeps the callback target selected by SYS_SETUINT
    // in this application dispatch table.  During early startup the table
    // contains the same generic vector trampoline that is present in the
    // SCU table; entering that trampoline as a callback would recursively
    // dispatch itself forever instead of returning to the interrupted code.
    static constexpr core::Address kApplicationDispatchTable = 0x0606D0ECU;
    static constexpr core::Address kDummyInterruptHandler = 0x06000610U;

    struct Stats {
        struct TimingProfile {
            // Calls is exact while profiling is enabled. Frequent paths are
            // timed once per kProfileSampleInterval calls; estimatedNanoseconds
            // scales their mean sample over Calls. VBlank paths sample every call.
            std::uint64_t calls{};
            std::uint64_t sampledCalls{};
            std::uint64_t sampledNanoseconds{};
            std::uint64_t estimatedNanoseconds{};
        };

        std::uint64_t elapsedBlocks{};
        std::uint64_t vblankEvents{};
        std::uint64_t vblankOutEvents{};
        std::uint64_t hblankEvents{};
        std::uint64_t timer0Events{};
        std::uint64_t cdSubcodeEvents{};
        std::uint64_t deliveredInterrupts{};
        std::uint64_t maskedEvents{};
        std::uint64_t droppedEvents{};
        TimingProfile scspBlockTick{};
        TimingProfile scspSchedulerTick{};
        TimingProfile mmioSchedulerTick{};
        TimingProfile scspVBlank{};
        TimingProfile videoVBlankOut{};
    };

    // A period of zero disables automatic VBlank generation. Events can still
    // be queued explicitly with requestVBlank().
    explicit ScuEventScheduler(std::size_t vblankPeriodBlocks = 0U) noexcept;

    void setVBlankPeriod(std::size_t periodBlocks) noexcept;
    void setProfilingEnabled(bool enabled) noexcept;
    [[nodiscard]] std::size_t vblankPeriod() const noexcept;
    void requestVBlank() noexcept;

    // Called from ProgramExecutionOptions::interrupt at each block boundary.
    // It advances time and peripheral clocks, raises VBlank status factors,
    // and returns the registered handler target when the SCU mask/SR allow
    // delivery. The old framebuffer-mailbox fallback is limited to period 0.
    [[nodiscard]] std::optional<sir::InterruptRequest> beforeBlock(
        const sir::BasicBlock& block, sir::MachineState& state,
        std::size_t step, MmioInterceptor& mmio);

    [[nodiscard]] const Stats& stats() const noexcept;

private:
    std::size_t vblankPeriodBlocks_{};
    std::size_t periodCounter_{};
    std::size_t hblankPeriodBlocks_{};
    std::size_t hblankCounter_{};
    std::size_t scanlineCounter_{};
    std::size_t cdSubcodePeriodBlocks_{};
    std::size_t cdSubcodeCounter_{};
    std::size_t vblankOutCountdown_{};
    std::size_t lastStep_{};
    bool hasStep_{};
    bool reportedMasked_{};
    bool requestedVBlank_{};
    bool profilingEnabled_{};
    Stats stats_{};
};

} // namespace saturn::platform
