#include "saturn/saturn/scsp.hpp"

#include "saturn/sir/interpreter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace saturn::platform {
namespace {

[[nodiscard]] std::uint32_t widthMask(std::uint8_t width) noexcept {
    return width == 1U ? 0xFFU : width == 2U ? 0xFFFFU : 0xFFFFFFFFU;
}

[[nodiscard]] bool inRange(core::Address address, core::Address base,
                            std::size_t size) noexcept {
    return address >= base && static_cast<std::uint64_t>(address - base) < size;
}

} // namespace

std::optional<std::uint32_t> ScspRuntime::read(core::Address address,
                                               std::uint8_t width) const {
    if (width != 1U && width != 2U && width != 4U) {
        return 0U;
    }
    if (inRange(address, kRamBase, kRamSize)) {
        return readBytes(ram_, static_cast<std::size_t>(address - kRamBase), width);
    }
    if (inRange(address, kRegistersBase, kRegistersSize)) {
        return readBytes(registers_, static_cast<std::size_t>(address - kRegistersBase),
                         width);
    }
    return std::nullopt;
}

bool ScspRuntime::write(core::Address address, std::uint8_t width,
                        std::uint32_t value) {
    if (width != 1U && width != 2U && width != 4U) {
        return true;
    }
    value &= widthMask(width);
    if (inRange(address, kRamBase, kRamSize)) {
        writeBytes(ram_, static_cast<std::size_t>(address - kRamBase), width, value);
        return true;
    }
    if (inRange(address, kRegistersBase, kRegistersSize)) {
        const auto offset = static_cast<std::size_t>(address - kRegistersBase);
        writeBytes(registers_, offset, width, value);
        if (offset == 0x400U) {
            const auto mask = value;
            for (std::size_t voice = 0; voice < voices_.size(); ++voice) {
                if ((mask & (1U << voice)) != 0U) {
                    voices_[voice].active = true;
                    voices_[voice].phase = 0.0F;
                    voices_[voice].sampleCursor = voices_[voice].sampleStart;
                }
            }
        } else if (offset == 0x404U) {
            const auto mask = value;
            for (std::size_t voice = 0; voice < voices_.size(); ++voice) {
                if ((mask & (1U << voice)) != 0U) voices_[voice].active = false;
            }
        } else if (offset < voices_.size() * 0x20U && (offset & 0x1FU) <= 0x1CU) {
            const auto voice = offset / 0x20U;
            const auto field = offset & 0x1FU;
            const auto word = static_cast<float>(value & 0xFFFFU);
            if (voice < voices_.size() && field == 0x00U) {
                voices_[voice].frequency = 55.0F + std::fmod(word, 1760.0F);
            } else if (voice < voices_.size() && field == 0x04U) {
                const auto attenuation = std::min<std::uint32_t>(value, 127U);
                voices_[voice].volume = 1.0F -
                    static_cast<float>(attenuation) / 127.0F;
            } else if (voice < voices_.size() && field == 0x08U) {
                voices_[voice].pan = std::clamp((word - 128.0F) / 128.0F, -1.0F, 1.0F);
            } else if (voice < voices_.size() && field == 0x0CU) {
                voices_[voice].sampleStart = static_cast<std::size_t>(value) &
                    (kRamSize - 1U);
                voices_[voice].sampleCursor = voices_[voice].sampleStart;
            } else if (voice < voices_.size() && field == 0x10U) {
                voices_[voice].sampleLoop = static_cast<std::size_t>(value) &
                    (kRamSize - 1U);
            } else if (voice < voices_.size() && field == 0x14U) {
                voices_[voice].sampleEnd = static_cast<std::size_t>(value) &
                    (kRamSize - 1U);
            } else if (voice < voices_.size() && field == 0x18U) {
                // A zero step keeps the compatibility sine voice.  Non-zero
                // values select PCM playback and are interpreted as a
                // 16.16 byte-address increment.
                voices_[voice].sample16 = (value & 1U) != 0U;
                voices_[voice].frequency = std::max(
                    1.0F, static_cast<float>((value >> 1U) & 0x7FFFU));
            }
        }
        return true;
    }
    return false;
}

void ScspRuntime::onSchedulerTick(sir::MachineState& state) {
    ++stats_.schedulerTicks;
    // Service a pending sound-driver batch before the SH-2's next poll.
    // The game sets bit 7 at 0x25A004E0 and waits for acknowledgement.
    // PCM commands are decoded here; playback advances on the audio clock.
    const auto commandOffset = static_cast<std::size_t>(kCommandStatus - kRamBase);
    if (commandOffset < ram_.size() && (ram_[commandOffset] & 0x80U) != 0U) {
        serviceSoundCommands();
        ram_[commandOffset] = static_cast<std::uint8_t>(ram_[commandOffset] & 0x7FU);
    }
    const auto request0 = state.read16BE(kRequest0) != 0U;
    const auto request1 = state.read16BE(kRequest1) != 0U;
    const auto requested = static_cast<std::uint8_t>((request0 ? 1U : 0U) |
                                                      (request1 ? 2U : 0U));
    // A falling request edge rearms that channel. Rockman also keeps request
    // asserted across commands and acknowledges completion by clearing ready,
    // so a cleared ready word is another valid rearm condition.
    activeRequests_ = static_cast<std::uint8_t>(activeRequests_ & requested);
    if (requested == 0U) {
        return;
    }
    const auto ready0 = state.read16BE(kReady0) != 0U;
    const auto ready1 = state.read16BE(kReady1) != 0U;
    const auto consumed = static_cast<std::uint8_t>((ready0 ? 0U : 1U) |
                                                     (ready1 ? 0U : 2U));
    const auto newRequests = static_cast<std::uint8_t>(
        requested & (static_cast<std::uint8_t>(~activeRequests_) | consumed));
    if (newRequests == 0U) {
        return;
    }

    // This is the observable part of the producer: the counter advances and
    // each newly requested completion word becomes non-zero so its polling
    // loop can pass.
    state.write16BE(kSequence,
                    static_cast<std::uint16_t>(state.read16BE(kSequence) + 1U));
    if ((newRequests & 1U) != 0U) {
        state.write16BE(kReady0, 1U);
    }
    if ((newRequests & 2U) != 0U) {
        state.write16BE(kReady1, 1U);
    }
    activeRequests_ = static_cast<std::uint8_t>(activeRequests_ | newRequests);
    ++stats_.requests;
    if ((newRequests & 1U) != 0U) ++stats_.completions;
    if ((newRequests & 2U) != 0U) ++stats_.completions;
}

const ScspRuntime::Stats& ScspRuntime::stats() const noexcept { return stats_; }

void ScspRuntime::configurePcmDirect(PcmStream& stream, std::uint8_t levelPan,
                                     std::uint16_t pitch, std::uint8_t totalLevel) noexcept {
    const auto octaveBits = static_cast<int>((pitch >> 11U) & 15U);
    const auto octave = octaveBits >= 8 ? octaveBits - 16 : octaveBits;
    stream.step = (1024U + (pitch & 1023U)) << 6U;
    stream.step = octave >= 0 ? stream.step << octave : stream.step >> -octave;
    const auto level = levelPan >> 5U;
    const auto gain = level == 0U ? 0.0F :
        std::pow(10.0F, -3.0F * (7 - level) / 10.0F) *
            std::exp2(-static_cast<float>(totalLevel) / 16.0F);
    const auto pan = levelPan & 31U;
    const auto attenuation = (pan & 15U) == 15U ? 0.0F :
        std::pow(10.0F, -3.0F * (pan & 15U) / 20.0F);
    stream.leftGain = gain * ((pan & 16U) == 0U ? attenuation : 1.0F);
    stream.rightGain = gain * ((pan & 16U) != 0U ? attenuation : 1.0F);
}

void ScspRuntime::serviceSoundCommands() {
    // ST-241: host interface pointer at 0x404, eight 16-byte commands,
    // PCM positions at host+0xA0 in units of 4096 samples. Decode PCM plus
    // initialization/master volume; sequence synthesis and DSP are absent.
    const auto host = static_cast<std::size_t>(readBytes(ram_, 0x404U, 4U));
    if (host < 0x500U || host > ram_.size() - 0x100U) return;
    for (std::size_t packet = host; packet < host + 0x80U; packet += 16U) {
        const auto command = ram_[packet];
        if (command != 0U) ++stats_.soundCommands[command];
        const auto number = ram_[packet + 2U] & 0x0FU;
        if (command == 0x10U) {
            // ST-241 section 6.10: each parameter acts only when equal to 1.
            // Keep PCM running when only sequences/effect output are reset.
            if (ram_[packet + 2U] == 1U) {
                for (auto& voice : voices_) voice.active = false;
            }
            if (ram_[packet + 3U] == 1U) {
                for (auto& stream : pcmStreams_) stream.active = false;
            }
            if (ram_[packet + 4U] == 1U) {
                registers_[0x217U] = 0U;
                registers_[0x237U] = 0U;
            }
            // This HLE has no DSP/effect output to reset for P4/P5.
        } else if (command == 0x82U) {
            masterVolume_ = ram_[packet + 2U] & 15U;
            registers_[0x401U] = static_cast<std::uint8_t>(
                (registers_[0x401U] & 0xF0U) | masterVolume_);
        } else if (number < pcmStreams_.size() && command == 0x85U) {
            for (std::size_t word = 0; word < stats_.lastPcmStart.size(); ++word) {
                stats_.lastPcmStart[word] = readBytes(ram_, packet + word * 4U, 4U);
            }
            PcmStream stream;
            stream.stereo = (ram_[packet + 2U] & 0x80U) != 0U;
            stream.sample16 = (ram_[packet + 2U] & 0x10U) == 0U;
            stream.start = readBytes(ram_, packet + 4U, 2U) << 4U;
            stream.samples = readBytes(ram_, packet + 6U, 2U);
            // The observed full-size buffer encodes 65536 samples as zero.
            if (stream.samples == 0U) stream.samples = 65536U;
            const auto bytes = stream.samples * (stream.sample16 ? 2U : 1U) *
                               (stream.stereo ? 2U : 1U);
            stream.active = stream.start < ram_.size() && bytes <= ram_.size() - stream.start;
            configurePcmDirect(stream, ram_[packet + 3U],
                static_cast<std::uint16_t>(readBytes(ram_, packet + 8U, 2U)),
                ram_[packet + 12U]);
            pcmStreams_[number] = stream;
            ram_[host + 0xA0U + number * 2U] = 0U;
            if (stream.active) ++stats_.pcmStarts;
            else ++stats_.pcmRejectedStarts;
        } else if (number < pcmStreams_.size() && command == 0x86U) {
            pcmStreams_[number].active = false;
        } else if (command == 0x8AU && ram_[packet + 2U] < pcmStreams_.size() &&
                   (ram_[packet + 6U] & 7U) == 0U &&
                   (ram_[packet + 7U] & 7U) == 0U) {
            // Direct-only parameter changes are supported. Leave requests that
            // route audio through the unimplemented DSP pending.
            auto& stream = pcmStreams_[ram_[packet + 2U]];
            if (stream.active) {
                configurePcmDirect(stream, ram_[packet + 3U],
                    static_cast<std::uint16_t>(readBytes(ram_, packet + 4U, 2U)),
                    ram_[packet + 8U]);
            }
        } else {
            // Preserve the existing compatibility path for unimplemented
            // commands; do not report their execution as completed here.
            continue;
        }
        // Acknowledge each supported block as well as the batch flag. Leaving
        // 0x85 pending makes the host resubmit it and reset playback forever.
        ram_[packet] = 0U;
    }
}

void ScspRuntime::onVBlank() {
    constexpr auto kSamplesPerVideoFrame = kAudioSampleRate / 60U;
    if (!realtimeAudioClock_) {
        renderAudioFrame(kSamplesPerVideoFrame);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (lastAudioTime_ == std::chrono::steady_clock::time_point{}) {
        lastAudioTime_ = now;
        renderAudioFrame(kSamplesPerVideoFrame);
        return;
    }

    // The mixer runs at 44.1 kHz, independently of the rate at which the
    // SH-2 reaches VBlank. Carry fractional samples so variable frame times
    // do not introduce long-term drift. Limit one catch-up block to 500 ms;
    // longer stalls are already audible and must not allocate huge buffers.
    const auto elapsed = std::chrono::duration<double>(now - lastAudioTime_).count();
    lastAudioTime_ = now;
    constexpr double kMaximumAudioBlockSeconds = 0.5;
    const auto boundedElapsed = std::clamp(elapsed, 0.0, kMaximumAudioBlockSeconds);
    const auto exactSamples = boundedElapsed * kAudioSampleRate + fractionalAudioSamples_;
    const auto samples = static_cast<std::size_t>(exactSamples);
    fractionalAudioSamples_ = exactSamples - static_cast<double>(samples);
    renderAudioFrame(std::max<std::size_t>(samples, 1U));
}

bool ScspRuntime::consumeAudio(AudioFrame& output) {
    if (!audioPending_) return false;
    output = std::move(lastAudioFrame_);
    audioPending_ = false;
    return true;
}

void ScspRuntime::setAudioSink(AudioFrameSink sink) {
    audioSink_ = std::move(sink);
}

void ScspRuntime::setRealtimeAudioClock(bool enabled) noexcept {
    realtimeAudioClock_ = enabled;
    lastAudioTime_ = {};
    fractionalAudioSamples_ = 0.0;
}

void ScspRuntime::renderAudioFrame(std::size_t samplesPerChannel) {
    lastAudioFrame_.sampleRate = kAudioSampleRate;
    lastAudioFrame_.channels = kAudioChannels;
    lastAudioFrame_.sequence = ++audioSequence_;
    lastAudioFrame_.samples.assign(samplesPerChannel * kAudioChannels, 0.0F);
    std::uint32_t active{};
    for (const auto& voice : voices_) active += voice.active ? 1U : 0U;
    for (const auto& stream : pcmStreams_) active += stream.active ? 1U : 0U;
    // Each MVOL decrement halves amplitude; zero mutes output, not the clock.
    const auto masterGain = masterVolume_ == 0U ? 0.0F :
        std::ldexp(1.0F, static_cast<int>(masterVolume_) - 15);
    for (std::size_t sample = 0; sample < samplesPerChannel; ++sample) {
        float left{};
        float right{};
        for (auto& voice : voices_) {
            if (!voice.active || voice.volume <= 0.0F) continue;
            float wave{};
            if (voice.sampleEnd > voice.sampleStart &&
                voice.sampleEnd <= ram_.size()) {
                const auto width = voice.sample16 ? 2U : 1U;
                if (voice.sampleCursor + width > voice.sampleEnd) {
                    voice.sampleCursor = voice.sampleLoop >= voice.sampleStart &&
                        voice.sampleLoop < voice.sampleEnd ? voice.sampleLoop :
                        voice.sampleStart;
                }
                if (voice.sample16) {
                    const auto value = static_cast<std::int16_t>(
                        static_cast<std::uint16_t>(ram_[voice.sampleCursor] << 8U) |
                        ram_[voice.sampleCursor + 1U]);
                    wave = static_cast<float>(value) / 32768.0F;
                } else {
                    wave = (static_cast<float>(ram_[voice.sampleCursor]) - 128.0F) /
                        128.0F;
                }
                voice.sampleCursor += width;
            } else {
                wave = std::sin(voice.phase * 6.28318530717958647692F);
                voice.phase = std::fmod(voice.phase + voice.frequency /
                                        static_cast<float>(kAudioSampleRate), 1.0F);
            }
            wave *= voice.volume;
            const auto leftGain = 0.5F * (1.0F - voice.pan);
            const auto rightGain = 0.5F * (1.0F + voice.pan);
            left += wave * leftGain;
            right += wave * rightGain;
        }
        for (auto& stream : pcmStreams_) {
            if (!stream.active) continue;
            const auto width = stream.sample16 ? 2U : 1U;
            const auto index = static_cast<std::size_t>(stream.position >> 16U);
            const auto pcm = [&](std::size_t address) {
                if (stream.sample16) {
                    const auto raw = readBytes(ram_, address, 2U);
                    const auto signedValue = raw >= 0x8000U ? static_cast<int>(raw) - 65536 :
                        static_cast<int>(raw);
                    return static_cast<float>(signedValue) / 32768.0F;
                }
                const auto raw = ram_[address];
                const auto signedValue = raw >= 128U ? static_cast<int>(raw) - 256 :
                    static_cast<int>(raw);
                return static_cast<float>(signedValue) / 128.0F;
            };
            const auto address = stream.start + index * width;
            // The Sega driver puts the right plane first for stereo PCM.
            // CPK's original transfer code uses this order; host WAV is L,R.
            left += pcm(address + (stream.stereo ? stream.samples * width : 0U)) * stream.leftGain;
            right += pcm(address) * stream.rightGain;
            stream.position = (stream.position + stream.step) %
                (static_cast<std::uint64_t>(stream.samples) << 16U);
        }
        lastAudioFrame_.samples[sample * 2U] = std::clamp(left * masterGain, -1.0F, 1.0F);
        lastAudioFrame_.samples[sample * 2U + 1U] = std::clamp(right * masterGain, -1.0F, 1.0F);
    }
    const auto host = static_cast<std::size_t>(readBytes(ram_, 0x404U, 4U));
    if (host >= 0x500U && host <= ram_.size() - 0x100U) {
        for (std::size_t number = 0; number < pcmStreams_.size(); ++number) {
            const auto& stream = pcmStreams_[number];
            if (!stream.active) continue;
            const auto position = static_cast<std::uint8_t>(stream.position >> 28U);
            auto& status = ram_[host + 0xA0U + number * 2U];
            if (status != position) ++stats_.pcmStatusUpdates;
            status = position;
        }
    }
    stats_.activeVoices = active;
    ++stats_.audioFrames;
    stats_.audioSamples += samplesPerChannel;
    audioPending_ = true;
    if (audioSink_) audioSink_(lastAudioFrame_);
}

std::uint32_t ScspRuntime::readBytes(const std::vector<std::uint8_t>& memory,
                                     std::size_t offset, std::uint8_t width) noexcept {
    if (offset >= memory.size() || offset + width > memory.size()) return 0U;
    std::uint32_t value{};
    for (std::uint8_t index = 0; index < width; ++index) {
        value = (value << 8U) | memory[offset + index];
    }
    return value;
}

void ScspRuntime::writeBytes(std::vector<std::uint8_t>& memory, std::size_t offset,
                             std::uint8_t width, std::uint32_t value) noexcept {
    if (offset >= memory.size() || offset + width > memory.size()) return;
    for (std::uint8_t index = 0; index < width; ++index) {
        const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
        memory[offset + index] = static_cast<std::uint8_t>(value >> shift);
    }
}

} // namespace saturn::platform
