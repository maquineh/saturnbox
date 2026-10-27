#pragma once

#include "saturn/core/address.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <vector>

namespace saturn::sir {
class MachineState;
}

namespace saturn::platform {

// Deterministic SCSP memory/register runtime plus the small SH-2/68K mailbox
// used by the Rockman runtime. The mailbox itself lives in high work RAM;
// the SCSP object owns only the peripheral window and services the mailbox at
// scheduler block boundaries.
class ScspRuntime final {
public:
    static constexpr core::Address kRamBase = 0x05A00000U;
    static constexpr std::size_t kRamSize = 0x80000U;
    // The SH-2 sound driver uses this byte as a level-triggered command
    // handshake.  The SH-2 sets bit 7 while a command is pending; the 68K
    // clears it after consuming the command.
    static constexpr core::Address kCommandStatus = kRamBase + 0x4E0U;
    static constexpr core::Address kRegistersBase = 0x05B00000U;
    static constexpr std::size_t kRegistersSize = 0x10000U;
    static constexpr core::Address kKeyOn = kRegistersBase + 0x400U;
    static constexpr core::Address kKeyOff = kRegistersBase + 0x404U;
    static constexpr std::uint32_t kAudioSampleRate = 44100U;
    static constexpr std::size_t kAudioChannels = 2U;

    struct AudioFrame {
        std::uint32_t sampleRate{kAudioSampleRate};
        std::size_t channels{kAudioChannels};
        std::uint64_t sequence{};
        std::vector<float> samples;
    };
    using AudioFrameSink = std::function<void(const AudioFrame&)>;

    // Work-area mailbox observed by the code polling at 0x06010722.
    static constexpr core::Address kSequence = 0x0605E790U;
    static constexpr core::Address kRequest0 = 0x0605E792U;
    static constexpr core::Address kReady0 = 0x0605E796U;
    static constexpr core::Address kReady1 = 0x0605E798U;
    static constexpr core::Address kRequest1 = 0x0605E79AU;

    struct Stats {
        std::uint64_t schedulerTicks{};
        std::uint64_t requests{};
        std::uint64_t completions{};
        std::uint64_t audioFrames{};
        std::uint64_t audioSamples{};
        std::uint32_t activeVoices{};
        std::uint64_t pcmStarts{};
        std::uint64_t pcmStatusUpdates{};
        std::uint64_t pcmRejectedStarts{};
        std::array<std::uint64_t, 256> soundCommands{};
        std::array<std::uint32_t, 4> lastPcmStart{};
    };

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) const;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                              std::uint32_t value);

    // Runs the deterministic 68K-side producer at a block boundary. A
    // request is acknowledged once until the SH-2 either clears its request
    // word or consumes the completion by clearing the corresponding ready
    // word. The latter supports the game's level-held request protocol.
    void onSchedulerTick(sir::MachineState& state);
    // Advance the audio clock at VBlank. Deterministic mode emits one 60 Hz
    // block; a real-time frontend can instead follow elapsed host time. The
    // runtime exposes stereo samples without depending on a platform mixer.
    void onVBlank();
    [[nodiscard]] bool consumeAudio(AudioFrame& output);
    void setAudioSink(AudioFrameSink sink);
    // Real-time frontends let the audio sample clock follow elapsed host time
    // so a slow video frame does not stretch the music timeline.
    void setRealtimeAudioClock(bool enabled) noexcept;

    [[nodiscard]] const Stats& stats() const noexcept;

private:
    [[nodiscard]] static std::uint32_t readBytes(const std::vector<std::uint8_t>& memory,
                                                  std::size_t offset,
                                                  std::uint8_t width) noexcept;
    static void writeBytes(std::vector<std::uint8_t>& memory, std::size_t offset,
                           std::uint8_t width, std::uint32_t value) noexcept;
    void renderAudioFrame(std::size_t samplesPerChannel);
    void serviceSoundCommands();

    // Sega sound-driver ABI, not an emulation of the 68K instruction stream.
    struct PcmStream {
        bool active{};
        bool stereo{};
        bool sample16{};
        std::size_t start{};
        std::uint32_t samples{};
        std::uint64_t position{}; // 16.16 samples, modulo the per-channel buffer
        std::uint32_t step{65536U};
        float leftGain{};
        float rightGain{};
    };
    static void configurePcmDirect(PcmStream& stream, std::uint8_t levelPan,
                                   std::uint16_t pitch, std::uint8_t totalLevel) noexcept;
    std::array<PcmStream, 8> pcmStreams_{};
    std::uint8_t masterVolume_{15U};

    struct Voice {
        bool active{};
        float phase{};
        std::size_t sampleCursor{};
        std::size_t sampleStart{};
        std::size_t sampleLoop{};
        std::size_t sampleEnd{};
        bool sample16{};
        float frequency{440.0F};
        float volume{};
        float pan{};
    };

    std::vector<std::uint8_t> ram_ = std::vector<std::uint8_t>(kRamSize);
    std::vector<std::uint8_t> registers_ = std::vector<std::uint8_t>(kRegistersSize);
    std::uint8_t activeRequests_{};
    Stats stats_{};
    std::array<Voice, 32> voices_{};
    std::uint64_t audioSequence_{};
    AudioFrame lastAudioFrame_{};
    bool audioPending_{};
    AudioFrameSink audioSink_;
    bool realtimeAudioClock_{};
    std::chrono::steady_clock::time_point lastAudioTime_{};
    double fractionalAudioSamples_{};
};

} // namespace saturn::platform
