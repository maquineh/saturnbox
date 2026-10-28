#pragma once

#include "saturn/saturn/scsp_dsp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace saturn::platform {

// Direct PCM path of the SCSP. Clock once per 44.1 kHz output sample.
// Implements the 32 slots with PCM8/PCM16 playback, the internal noise source,
// pitch and amplitude LFOs, phase modulation between slots and the effect DSP
// send/return. Timers and MIDI live in ScspTiming; DMA is outside this model.
class ScspSynth final {
public:
    static constexpr std::size_t kSlotCount = 32;
    static constexpr std::size_t kRegisterBytes = 0x1000;
    // 64 entries of recent slot output used as the FM/modulation ring buffer.
    static constexpr std::size_t kRingBufferSize = 64;

    struct Stats {
        std::uint64_t renderedSamples{};
        std::uint64_t keyOns{};
        std::uint64_t keyOffs{};
        // Bit per slot: which slots are sounding after the last sample. Useful
        // to tell a music bed from an effect or a voice slot when validating.
        std::uint32_t activeSlotMask{};
        // Activity counters for sources the previous model treated as absent.
        std::uint64_t noiseSamples{};
        std::uint64_t pitchLfoSamples{};
        std::uint64_t amplitudeLfoSamples{};
        std::uint64_t fmSamples{};
        std::uint64_t dspSendSamples{};
        std::uint64_t dspEffectSamples{};
        // SSCTL=3 is documented as unusable. It stays silent and counted.
        std::uint64_t unsupportedSourceSamples{};
        std::uint64_t outOfRangeSamples{};
        std::uint64_t invalidLoops{};
    };

    ScspSynth();
    void reset();
    [[nodiscard]] bool write(std::uint32_t offset, std::uint8_t width,
                             std::uint32_t value);
    [[nodiscard]] std::optional<std::uint32_t> read(std::uint32_t offset,
                                                   std::uint8_t width) const;
    // Sound RAM is writable because the effect DSP keeps its delay lines there.
    [[nodiscard]] std::array<float, 2> renderSample(std::span<std::uint8_t> soundRam);
    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const ScspDsp& dsp() const noexcept { return dsp_; }

private:
    enum class Envelope : std::uint8_t { Attack, Decay1, Decay2, Release };
    struct Slot {
        bool active{};
        bool backwards{};
        bool loopEntered{};
        bool pcm8{};
        bool ringWrite{};
        Envelope envelope{Envelope::Release};
        std::int64_t position{}; // 12 fractional bits, relative sample address.
        std::uint32_t step{4096};
        double level{}; // 0..1023, where 1023 is full level.
        std::array<double, 4> rates{};
        std::array<float, 2> gain{};
        float totalLevelGain{1.0F}; // TL attenuation alone.
        float sendGain{};           // IMXL send level into the DSP.
        std::uint8_t isel{};
        std::uint8_t imxl{};
        std::uint8_t effectSendLevel{};
        std::uint8_t effectPan{};
        std::uint8_t modulationLevel{};
        std::uint8_t modulationX{};
        std::uint8_t modulationY{};
        std::uint32_t pitchLfoPhase{};
        std::uint32_t pitchLfoStep{};
        std::uint32_t amplitudeLfoPhase{};
        std::uint32_t amplitudeLfoStep{};
        std::uint8_t pitchLfoShape{};
        std::uint8_t pitchLfoScale{};
        std::uint8_t amplitudeLfoShape{};
        std::uint8_t amplitudeLfoScale{};
    };
    [[nodiscard]] std::uint16_t word(std::size_t offset) const noexcept;
    void refreshSlot(std::size_t index);
    void refreshLfo(Slot& slot, std::size_t index);
    void executeKeyOn();
    void stop(std::size_t index);
    void advancePosition(std::size_t index, std::uint32_t step);
    [[nodiscard]] float envelopeGain(std::size_t index);
    [[nodiscard]] float pcm(std::size_t index, std::int64_t sample,
                            std::span<std::uint8_t> ram);
    [[nodiscard]] std::int32_t nextNoise() noexcept;
    std::array<std::uint8_t, kRegisterBytes> registers_{};
    std::array<Slot, kSlotCount> slots_{};
    std::array<std::int32_t, kRingBufferSize> ringBuffer_{};
    std::size_t ringPointer_{};
    std::uint16_t monitor_{};
    std::uint16_t noiseState_{0x4A3FU};
    ScspDsp dsp_{};
    Stats stats_{};
};

} // namespace saturn::platform
