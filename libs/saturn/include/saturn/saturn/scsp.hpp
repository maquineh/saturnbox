#pragma once

#include "saturn/core/address.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace saturn::sir {
class MachineState;
}

namespace saturn::platform {

// Deterministic SCSP memory/register runtime. Rockman's VDP1 field-sync
// mailbox normally belongs to the guest VBlank callbacks; an explicit legacy
// fallback remains available to harnesses that disable periodic VBlank.
class ScspRuntime final {
public:
    class DriverError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
    ScspRuntime();
    ~ScspRuntime();
    ScspRuntime(const ScspRuntime&) = delete;
    ScspRuntime& operator=(const ScspRuntime&) = delete;
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

    // Guest-owned VDP1 field-sync words exposed for diagnostics. The SCSP
    // runtime must not synthesize acknowledgements for these addresses.
    static constexpr core::Address kSequence = 0x0605E790U;
    static constexpr core::Address kRequest0 = 0x0605E792U;
    static constexpr core::Address kReady0 = 0x0605E796U;
    static constexpr core::Address kReady1 = 0x0605E798U;
    static constexpr core::Address kRequest1 = 0x0605E79AU;

    struct Stats {
        // Sound command packet captured from the host queue, in order. Only the
        // sequence-start command (0x01) is recorded: it carries the bank and
        // sequence number, which is what identifies a cue.
        struct SequenceRequest {
            std::uint64_t schedulerTick{};
            std::uint8_t command{};
            std::uint8_t control{};
            std::uint8_t bank{};
            std::uint8_t sequence{};
        };
        static constexpr std::size_t kMaxSoundRequests = 16384;
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
        std::uint64_t driverCycles{}, driverResets{}, driverRegisterWrites{};
        std::uint32_t driverPc{};
        std::uint32_t slotMask{}; // Bit per sounding slot, for voice triage.
        std::uint64_t driverUnmappedReads{}, driverUnmappedWrites{}, slotKeyOns{}, slotKeyOffs{};
        // Sound-CPU core calls, i.e. how finely the 68000 is interleaved.
        std::uint64_t soundCpuCalls{};
        // Sound sources the earlier model counted as unsupported are now
        // synthesized; these counters show how often each path is used.
        std::uint64_t noiseSamples{}, pitchLfoSamples{}, amplitudeLfoSamples{};
        std::uint64_t fmSamples{}, dspSendSamples{}, dspEffectSamples{}, dspSteps{};
        std::uint64_t sequenceRequests{}; // Only 0x01, even beyond the ring.
        std::uint64_t soundRequests{};    // Every command packet, in the ring.
        std::vector<SequenceRequest> lastSoundRequests;
        // Scheduler steps elapsed, the same unit --pad-sequence uses. Recorded
        // with each request so a capture can be aligned to scripted input.
        std::uint64_t schedulerSteps{};
        std::uint64_t unsupportedSourceSamples{}, slotOutOfRangeSamples{};
        // SCSP DMA and MIDI activity.
        std::uint64_t dmaTransfers{}, dmaWords{}, midiBytesOut{}, midiBytesIn{};
        // Sub-frame audio clock: one tick per scheduler step, and the number of
        // samples rendered ahead of the VBlank that publishes the frame.
        std::uint64_t audioBlockTicks{}, audioSubFrameSamples{};
    };

    // Optional real 68000 + hardware slot path. Must be selected before
    // loading/starting the guest sound system; HLE remains the default.
    [[nodiscard]] static bool driverCpuAvailable() noexcept;
    [[nodiscard]] bool enableDriverCpu();
    [[nodiscard]] bool driverCpuEnabled() const noexcept;
    void setSoundCpuEnabled(bool enabled) noexcept;

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) const;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                              std::uint32_t value);

    // Services a pending PCM HLE command batch at a guest block boundary.
    // With the 68000 backend enabled, the real sound CPU consumes the batch.
    void onSchedulerTick();
    // Compatibility service for headless harnesses with no periodic VBlank.
    // Interactive gameplay must leave these words to the guest's field IRQs.
    void serviceFramebufferMailboxFallback(sir::MachineState& state);
    // Advance the audio clock at VBlank. Deterministic mode emits one 60 Hz
    // block; a real-time frontend can instead follow elapsed host time. The
    // runtime exposes stereo samples without depending on a platform mixer.
    void onVBlank();
    [[nodiscard]] bool consumeAudio(AudioFrame& output);
    void setAudioSink(AudioFrameSink sink);
    // Real-time frontends let the audio sample clock follow elapsed host time
    // so a slow video frame does not stretch the music timeline.
    void setRealtimeAudioClock(bool enabled) noexcept;
    // Deterministic mode renders the frame's samples across the scheduler
    // steps instead of one burst at VBlank, so SH-2 register writes, sound
    // commands and the sound CPU interleave within a frame. The scheduler
    // reports its step count per frame; zero keeps the former VBlank burst.
    void setAudioFrameBlocks(std::size_t blocksPerFrame) noexcept;
    // One scheduler step of sound time. Called by the event scheduler, or by
    // tests that drive the audio clock directly.
    void onBlockTick();
    // Sound-CPU time advances with the audio clock: each step carries a share
    // of the frame's 188,160 cycles (256 per rendered sample). Chunks smaller
    // than the configured quantum wait for the next step, or for the VBlank
    // flush, so a 14,000-step frame does not cost 14,000 core calls.
    static constexpr std::uint64_t kDefaultSoundCpuChunkCycles = 64;
    // How finely the sound CPU is interleaved with the rest of the emulation.
    // One cycle is the floor: the budget is consumed once per core call, so a
    // smaller chunk means more calls for the same 256 cycles per sample.
    void setSoundCpuChunk(std::uint64_t cycles) noexcept;
    [[nodiscard]] std::uint64_t soundCpuChunk() const noexcept {
        return soundCpuChunkCycles_;
    }

    [[nodiscard]] const Stats& stats() const noexcept;

private:
    [[nodiscard]] static std::uint32_t readBytes(const std::vector<std::uint8_t>& memory,
                                                  std::size_t offset,
                                                  std::uint8_t width) noexcept;
    static void writeBytes(std::vector<std::uint8_t>& memory, std::size_t offset,
                           std::uint8_t width, std::uint32_t value) noexcept;
    void renderAudioFrame(std::size_t samplesPerChannel);
    void beginAudioFrame(std::size_t samplesPerChannel);
    void renderSamples(std::size_t count);
    void finishAudioFrame();
    void advanceSoundCpu(std::uint64_t targetCycles, bool flush = false);
    void recordSequenceRequest(std::size_t packet);
    [[nodiscard]] bool blockClockActive() const noexcept {
        // Device playback must not move the real 68000 ahead of the SH-2.
        // Keep driver execution/register writes on the same guest clock in
        // headless and windowed runs; wall time is only a legacy HLE option.
        return audioFrameBlocks_ != 0U && (driver_ || !realtimeAudioClock_);
    }
    void serviceSoundCommands();
    struct DriverBackend;
    std::unique_ptr<DriverBackend> driver_;
    bool soundCpuEnabled_{};

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
    // Sub-frame clock state. `audioFrameOpen_` guards a partially rendered
    // frame between the first scheduler step after VBlank and the VBlank that
    // publishes it.
    std::size_t audioFrameBlocks_{};
    std::size_t frameBlockCount_{};
    std::size_t frameSampleCount_{};
    bool audioFrameOpen_{};
    // Sound-CPU cycles already executed in the current frame. Kept separate
    // from driverCycles so a frame can finish its exact 256-cycles-per-sample
    // budget even when the last scheduler step lands short.
    std::uint64_t frameSoundCpuCycles_{};
    std::uint64_t soundCpuChunkCycles_{kDefaultSoundCpuChunkCycles};
};

} // namespace saturn::platform
