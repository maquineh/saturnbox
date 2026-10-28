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
    // MIDI interface: 0x404 is the status/MIBUF word, 0x406 the output buffer.
    static constexpr std::uint16_t kMidiStatus = 0x404U;
    static constexpr std::uint16_t kMidiOutput = 0x406U;
    static constexpr std::size_t kMidiFifoSize = 32U;
    // Interrupt factor bits: 3 is MIDI and 4 is DMA completion.
    static constexpr std::size_t kMidiPendingBit = 3U;
    static constexpr std::size_t kDmaPendingBit = 4U;

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
        std::uint64_t midiBytesOut{};
        std::uint64_t midiBytesIn{};
        std::uint64_t midiOverflows{};
        std::uint64_t dmaCompletions{};
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

    // The Saturn MIDI port has no device in this model, so input bytes come
    // from the host and output bytes are drained one per audio sample. Serial
    // timing (31250 baud) is not modeled.
    void pushMidiInput(std::uint8_t byte) noexcept;
    [[nodiscard]] std::size_t midiInputPending() const noexcept;
    [[nodiscard]] std::uint8_t takeMidiOutput() noexcept;
    // Hardware interrupt factors outside the timer block: MIDI (3) and DMA
    // completion (4). A completion also increments the DMA counter.
    void setPendingBit(std::size_t bit, bool pending) const noexcept;
    void notifyDmaCompletion() noexcept;

private:
    [[nodiscard]] static bool isMapped(std::uint16_t offset) noexcept;
    [[nodiscard]] std::uint8_t readByte(std::uint16_t offset) const noexcept;
    void writeByte(std::uint16_t offset, std::uint8_t value) noexcept;
    void advanceTimer(std::size_t index, std::uint64_t sampleCount) noexcept;
    void setPending(std::uint16_t mask) noexcept;

    // Counters and pending bits are updated from const register reads (MIDI
    // MIBUF consumption), so the snapshot is mutable.
    mutable Stats stats_{};
    std::array<std::uint8_t, 3> interruptLevels_{};
    // MIDI FIFOs are drained from const register reads, so they are mutable.
    mutable std::array<std::uint8_t, kMidiFifoSize> midiInput_{};
    mutable std::uint8_t midiInputRead_{};
    mutable std::uint8_t midiInputWrite_{};
    mutable std::uint8_t midiInputCount_{};
    mutable bool midiInputOverflow_{};
    mutable std::array<std::uint8_t, kMidiFifoSize> midiOutput_{};
    mutable std::uint8_t midiOutputRead_{};
    mutable std::uint8_t midiOutputWrite_{};
    mutable std::uint8_t midiOutputCount_{};
};

} // namespace saturn::platform
