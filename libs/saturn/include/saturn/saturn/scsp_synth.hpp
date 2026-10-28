#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace saturn::platform {

// Direct PCM path of the SCSP. Clock once per 44.1 kHz output sample.
// Timers, DMA, MIDI, DSP, modulation and noise are outside this device model.
class ScspSynth final {
public:
    static constexpr std::size_t kSlotCount = 32;
    static constexpr std::size_t kRegisterBytes = 0x1000;
    struct Stats {
        std::uint64_t renderedSamples{};
        std::uint64_t keyOns{};
        std::uint64_t keyOffs{};
        // Unsupported counters count active slot-samples, not register writes.
        std::uint64_t unsupportedSourceSamples{};
        std::uint64_t unsupportedLfoSamples{};
        std::uint64_t unsupportedFmSamples{};
        std::uint64_t unsupportedDspSamples{};
        std::uint64_t outOfRangeSamples{};
        std::uint64_t invalidLoops{};
    };

    ScspSynth();
    void reset();
    [[nodiscard]] bool write(std::uint32_t offset, std::uint8_t width,
                             std::uint32_t value);
    [[nodiscard]] std::optional<std::uint32_t> read(std::uint32_t offset,
                                                   std::uint8_t width) const;
    [[nodiscard]] std::array<float, 2> renderSample(std::span<const std::uint8_t> soundRam);
    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    enum class Envelope : std::uint8_t { Attack, Decay1, Decay2, Release };
    struct Slot {
        bool active{};
        bool backwards{};
        bool loopEntered{};
        Envelope envelope{Envelope::Release};
        std::int64_t position{}; // 12 fractional bits, relative sample address.
        std::uint32_t step{4096};
        double level{}; // 0..1023, where 1023 is full level.
        std::array<double, 4> rates{};
        std::array<float, 2> gain{};
    };
    [[nodiscard]] std::uint16_t word(std::size_t offset) const noexcept;
    void refreshSlot(std::size_t index);
    void executeKeyOn();
    void stop(std::size_t index);
    void advancePosition(std::size_t index);
    [[nodiscard]] float envelopeGain(std::size_t index);
    [[nodiscard]] float pcm(std::size_t index, std::int64_t sample,
                            std::span<const std::uint8_t> ram);
    std::array<std::uint8_t, kRegisterBytes> registers_{};
    std::array<Slot, kSlotCount> slots_{};
    std::uint16_t monitor_{};
    Stats stats_{};
};

} // namespace saturn::platform
