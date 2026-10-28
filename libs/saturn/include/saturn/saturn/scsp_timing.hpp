#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace saturn::platform {

// Sample-clocked subset of the SCSP timer and sound-CPU interrupt controller.
// The sound CPU clock is intentionally outside this device model.
class ScspTiming final {
public:
    static constexpr std::uint16_t kTimerA = 0x418U;
    static constexpr std::uint16_t kTimerB = 0x41AU;
    static constexpr std::uint16_t kTimerC = 0x41CU;
    static constexpr std::uint16_t kScieb = 0x41EU;
    static constexpr std::uint16_t kScipd = 0x420U;
    static constexpr std::uint16_t kScire = 0x422U;
    static constexpr std::uint16_t kScilv0 = 0x424U;
    static constexpr std::uint16_t kScilv1 = 0x426U;
    static constexpr std::uint16_t kScilv2 = 0x428U;

    struct TimerStats {
        std::uint8_t count{};
        std::uint8_t prescaler{};
        std::uint8_t sampleRemainder{};
        bool running{};
        std::uint64_t expirations{};
    };

    struct Stats {
        std::array<TimerStats, 3> timers{};
        std::uint64_t samplesAdvanced{};
        std::uint16_t interruptEnable{};
        std::uint16_t interruptPending{};
    };

    // Offsets are relative to the SCSP register window. Values use big-endian
    // byte lanes for supported widths 1, 2, and 4.
    [[nodiscard]] std::optional<std::uint32_t> read(std::uint16_t offset,
                                                    std::uint8_t width) const;
    [[nodiscard]] bool write(std::uint16_t offset, std::uint8_t width,
                             std::uint32_t value);

    // Advances the three counters from the SCSP's 44.1 kHz audio sample clock.
    void advanceSamples(std::uint64_t count) noexcept;
    [[nodiscard]] std::uint8_t irqLevel() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept;

private:
    [[nodiscard]] static bool isMapped(std::uint16_t offset) noexcept;
    [[nodiscard]] std::uint8_t readByte(std::uint16_t offset) const noexcept;
    void writeByte(std::uint16_t offset, std::uint8_t value) noexcept;
    void advanceTimer(std::size_t index, std::uint64_t sampleCount) noexcept;
    void setPending(std::uint16_t mask) noexcept;

    Stats stats_{};
    std::array<std::uint8_t, 3> interruptLevels_{};
};

} // namespace saturn::platform
