// Envelope timing tables, LFO tables and amplitude/pan equations adapted from
// MAME SCSP. Copyright ElSemi, R. Belmont. BSD-3-Clause; see
// third_party/MAME-SCSP-LICENSE.txt.
#include "saturn/saturn/scsp_synth.hpp"

#include <algorithm>
#include <cmath>

namespace saturn::platform {
namespace {
constexpr std::int64_t kOne = 4096;
constexpr std::array<double, 64> kAttackMs{
    100000,100000,8100,6900,6000,4800,4000,3400,3000,2400,2000,1700,1500,
    1200,1000,860,760,600,500,430,380,300,250,220,190,150,130,110,95,
    76,63,55,47,38,31,27,24,19,15,13,12,9.4,7.9,6.8,6,4.7,3.8,3.4,3,2.4,
    2,1.8,1.6,1.3,1.1,0.93,0.85,0.65,0.53,0.44,0.40,0.35,0,0};
constexpr std::array<double, 64> kDecayMs{
    100000,100000,118200,101300,88600,70900,59100,50700,44300,35500,29600,
    25300,22200,17700,14800,12700,11100,8900,7400,6300,5500,4400,3700,3200,
    2800,2200,1800,1600,1400,1100,920,790,690,550,460,390,340,270,230,200,
    170,140,110,98,85,68,57,49,43,34,28,25,22,18,14,12,11,8.5,7.1,6.1,5.4,4.3,3.6,3.1};
constexpr std::array<float, 8> kTlDb{0.4F,0.8F,1.5F,3,6,12,24,48};
// LFO rates in Hz, depth in cents (pitch) and dB (amplitude).
constexpr std::array<float, 32> kLfoFrequency{
    0.17F,0.19F,0.23F,0.27F,0.34F,0.39F,0.45F,0.55F,0.68F,0.78F,0.92F,1.10F,
    1.39F,1.60F,1.87F,2.27F,2.87F,3.31F,3.92F,4.79F,6.15F,7.18F,8.60F,10.8F,
    14.4F,17.2F,21.5F,28.7F,43.1F,57.4F,86.1F,172.3F};
constexpr std::array<float, 8> kPitchDepthCents{0.0F,7.0F,13.5F,27.0F,55.0F,112.0F,230.0F,494.0F};
constexpr std::array<float, 8> kAmplitudeDepthDb{0.0F,0.4F,0.8F,1.5F,3.0F,6.0F,12.0F,24.0F};

struct LfoTables {
    std::array<std::array<std::int16_t, 256>, 4> pitchWaveforms{};
    std::array<std::array<std::uint8_t, 256>, 4> amplitudeWaveforms{};
    std::array<std::array<std::uint16_t, 256>, 8> pitchScales{};     // 8.8 ratio.
    std::array<std::array<std::uint16_t, 256>, 8> amplitudeScales{}; // 0..256.
};

// MAME builds the random LFO waveform with the host RNG. A fixed xorshift keeps
// the tables deterministic so captures stay reproducible.
[[nodiscard]] const LfoTables& lfoTables() {
    static const LfoTables tables = [] {
        LfoTables built{};
        std::uint32_t random = 0x2545F491U;
        for (std::size_t index = 0; index < 256; ++index) {
            const auto sawAmplitude = static_cast<std::uint8_t>(255U - index);
            const auto sawPitch = static_cast<std::int16_t>(
                index < 128U ? static_cast<int>(index) : static_cast<int>(index) - 256);
            const auto squareAmplitude = static_cast<std::uint8_t>(index < 128U ? 255 : 0);
            const auto squarePitch = static_cast<std::int16_t>(index < 128U ? 127 : -128);
            const auto triangleAmplitude = static_cast<std::uint8_t>(
                index < 128U ? 255U - index * 2U : index * 2U - 256U);
            int trianglePitch = 0;
            if (index < 64U) trianglePitch = static_cast<int>(index) * 2;
            else if (index < 128U) trianglePitch = 255 - static_cast<int>(index) * 2;
            else if (index < 192U) trianglePitch = 256 - static_cast<int>(index) * 2;
            else trianglePitch = static_cast<int>(index) * 2 - 511;
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            const auto noiseAmplitude = static_cast<std::uint8_t>(random & 0xFFU);
            const auto noisePitch = static_cast<std::int16_t>(128 - noiseAmplitude);
            built.pitchWaveforms[0][index] = sawPitch;
            built.pitchWaveforms[1][index] = squarePitch;
            built.pitchWaveforms[2][index] = static_cast<std::int16_t>(trianglePitch);
            built.pitchWaveforms[3][index] = noisePitch;
            built.amplitudeWaveforms[0][index] = sawAmplitude;
            built.amplitudeWaveforms[1][index] = squareAmplitude;
            built.amplitudeWaveforms[2][index] = triangleAmplitude;
            built.amplitudeWaveforms[3][index] = noiseAmplitude;
        }
        for (std::size_t shape = 0; shape < 8; ++shape) {
            for (int value = -128; value < 128; ++value) {
                const auto cents = kPitchDepthCents[shape] *
                    static_cast<float>(value) / 128.0F;
                const auto ratio = std::pow(2.0F, cents / 1200.0F);
                built.pitchScales[shape][static_cast<std::size_t>(value + 128)] =
                    static_cast<std::uint16_t>(std::lround(256.0F * ratio));
            }
            for (std::size_t value = 0; value < 256; ++value) {
                const auto db = -kAmplitudeDepthDb[shape] *
                    static_cast<float>(value) / 256.0F;
                built.amplitudeScales[shape][value] = static_cast<std::uint16_t>(
                    std::lround(256.0F * std::pow(10.0F, db / 20.0F)));
            }
        }
        return built;
    }();
    return tables;
}

float dbGain(float db) { return std::pow(10.0F, -db / 20.0F); }
bool validAccess(std::uint32_t offset, std::uint8_t width) {
    return (width == 1 || width == 2 || width == 4)
        && offset < ScspSynth::kRegisterBytes
        && width <= ScspSynth::kRegisterBytes - offset;
}
std::int64_t positiveMod(std::int64_t value, std::int64_t modulus) {
    const auto result = value % modulus;
    return result < 0 ? result + modulus : result;
}
// Pan attenuator shared by the direct path, the DSP send and the effect return.
float panGain(std::uint16_t pan) {
    return (pan & 15U) == 15U ? 0.0F : dbGain(static_cast<float>((pan & 15U) * 3U));
}
} // namespace

ScspSynth::ScspSynth() { reset(); }
void ScspSynth::reset() {
    registers_.fill(0);
    slots_.fill(Slot{});
    ringBuffer_.fill(0);
    ringPointer_ = 0;
    monitor_ = 0x7f; // Release at minimum level.
    noiseState_ = 0x4A3F;
    dsp_.reset();
    dmaMemory_ = 0;
    dmaRegister_ = 0;
    dmaLength_ = 0;
    dmaDirection_ = false;
    dmaGate_ = false;
    dmaActive_ = false;
    dmaPending_ = false;
    soundRam_ = {};
    stats_ = {};
}
std::uint16_t ScspSynth::word(std::size_t offset) const noexcept {
    return static_cast<std::uint16_t>((registers_[offset] << 8U) | registers_[offset + 1]);
}
bool ScspSynth::write(std::uint32_t offset, std::uint8_t width, std::uint32_t value) {
    if (!validAccess(offset, width)) return false;
    for (unsigned i = 0; i < width; ++i)
        registers_[offset + i] = static_cast<std::uint8_t>(value >> ((width - 1U - i) * 8U));
    // Apply a bus write atomically, including the low SA word on a long write.
    if (offset < 0x400) {
        const auto last = std::min<std::uint32_t>(offset + width - 1, 0x3ff) / 0x20;
        for (auto index = offset / 0x20; index <= last; ++index) refreshSlot(index);
        for (auto index = offset / 0x20; index <= last; ++index) {
            const auto control = index * 0x20;
            if (offset < control + 2 && offset + width > control && (word(control) & 0x1000)) {
                executeKeyOn();
                registers_[control] &= static_cast<std::uint8_t>(~0x10U);
            }
        }
        return true;
    }
    // MEM4B selects the DSP delay buffer base (RBP) and length (RBL).
    if (offset <= 0x403U && offset + width > 0x402U) {
        const auto memoryMode = word(0x402);
        dsp_.setRingBuffer(static_cast<std::uint16_t>(memoryMode & 0x3FU),
                           static_cast<std::uint32_t>(8U * 1024U) << ((memoryMode >> 7U) & 3U));
    }
    // SCSP DMA: parameter words at 0x412-0x417. Writing DEXE starts the
    // transfer immediately when the sound-RAM image is already known.
    if (offset < 0x418U && offset + width > 0x412U) {
        const auto control = word(0x414);
        dmaMemory_ = ((static_cast<std::uint32_t>(control) & 0xF000U) << 4U) |
                     (static_cast<std::uint32_t>(word(0x412)) & 0xFFFEU);
        dmaRegister_ = static_cast<std::uint32_t>(control) & 0x0FFEU;
        const auto length = word(0x416);
        dmaLength_ = static_cast<std::uint32_t>(length) & 0x0FFEU;
        dmaDirection_ = (length & 0x2000U) != 0U;
        dmaGate_ = (length & 0x4000U) != 0U;
        if ((length & 0x1000U) != 0U) requestDma();
    }
    if (offset < 0xEE4U && offset + width > 0x700U) {
        const auto first = static_cast<std::uint16_t>(std::max<std::uint32_t>(offset, 0x700U) & ~1U);
        const auto last = static_cast<std::uint16_t>((offset + width - 1U) & ~1U);
        for (auto at = first; at <= last && at < 0xEE4U; at = static_cast<std::uint16_t>(at + 2U)) {
            dsp_.registerWord(at, word(at));
        }
    }
    return true;
}
std::optional<std::uint32_t> ScspSynth::read(std::uint32_t offset, std::uint8_t width) const {
    if (!validAccess(offset, width)) return std::nullopt;
    std::uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
        const auto address = offset + i;
        const auto byte = address == 0x408
            ? static_cast<std::uint8_t>(monitor_ >> 8)
            : address == 0x409 ? static_cast<std::uint8_t>(monitor_)
            : ScspDsp::isReadableRegister(static_cast<std::uint16_t>(address))
                ? dsp_.readByte(static_cast<std::uint16_t>(address)).value_or(0U)
            : registers_[address];
        result = (result << 8U) | byte;
    }
    return result;
}
void ScspSynth::refreshSlot(std::size_t index) {
    auto& slot = slots_[index];
    const auto base = index * 0x20;
    const auto pitch = word(base + 0x10);
    int octave = (pitch >> 11) & 15;
    if (octave & 8) octave -= 16;
    const auto frequency = static_cast<std::uint32_t>((pitch & 1023) + 1024);
    slot.step = octave + 2 >= 0 ? frequency << (octave + 2) : frequency >> (-octave - 2);
    const auto eg1 = word(base + 8);
    const auto eg2 = word(base + 10);
    const int keyRate = (eg2 >> 10) & 15;
    const int rateBase = keyRate == 15 ? 0 : octave + 2 * keyRate + ((pitch >> 9) & 1);
    const std::array<unsigned, 4> rates{eg1 & 31U, (eg1 >> 6) & 31U,
                                       (eg1 >> 11) & 31U, eg2 & 31U};
    for (std::size_t i = 0; i < rates.size(); ++i) {
        const auto rate = std::clamp(rateBase + 2 * static_cast<int>(rates[i]), 0, 63);
        const auto ms = i == 0 ? kAttackMs[rate] : kDecayMs[rate];
        slot.rates[i] = rate < 2 || (i == 2 && rates[i] == 0) ? 0
            : ms == 0 ? 1024 : 1023000.0 / (44100.0 * ms);
    }
    const auto level = word(base + 0x0c);
    float attenuation = 0;
    if (!(level & 0x100))
        for (unsigned bit = 0; bit < 8; ++bit)
            if (level & (1U << bit)) attenuation += kTlDb[bit];
    slot.totalLevelGain = dbGain(attenuation);
    const auto mix = word(base + 0x16);
    const auto send = (mix >> 13) & 7;
    const auto pan = (mix >> 8) & 31;
    const auto gain = send ? dbGain(attenuation + static_cast<float>((7 - send) * 6)) : 0.0F;
    const auto singlePan = panGain(static_cast<std::uint16_t>(pan));
    slot.gain = pan < 16 ? std::array<float, 2>{gain * singlePan, gain}
                         : std::array<float, 2>{gain, gain * singlePan};
    slot.effectSendLevel = static_cast<std::uint8_t>((mix >> 5) & 7);
    slot.effectPan = static_cast<std::uint8_t>(mix & 31);
    const auto route = word(base + 0x14);
    slot.isel = static_cast<std::uint8_t>((route >> 3) & 15);
    slot.imxl = static_cast<std::uint8_t>(route & 7);
    slot.sendGain = slot.imxl == 0 ? 0.0F
        : dbGain(attenuation + static_cast<float>((7 - slot.imxl) * 6));
    const auto modulation = word(base + 0x0e);
    slot.modulationLevel = static_cast<std::uint8_t>((modulation >> 12) & 15);
    slot.modulationX = static_cast<std::uint8_t>((modulation >> 6) & 63);
    slot.modulationY = static_cast<std::uint8_t>(modulation & 63);
    slot.pcm8 = (word(base) & 0x10) != 0;
    slot.ringWrite = (level & 0x200) == 0;
    refreshLfo(slot, index);
}
void ScspSynth::refreshLfo(Slot& slot, std::size_t index) {
    const auto base = index * 0x20;
    const auto lfo = word(base + 0x12);
    const auto frequency = (lfo >> 10) & 31;
    slot.pitchLfoShape = static_cast<std::uint8_t>((lfo >> 8) & 3);
    slot.amplitudeLfoShape = static_cast<std::uint8_t>((lfo >> 3) & 3);
    slot.pitchLfoScale = static_cast<std::uint8_t>((lfo >> 5) & 7);
    slot.amplitudeLfoScale = static_cast<std::uint8_t>(lfo & 7);
    // MAME: phase_step = (1 << 8) * (rate * 256 / 44100).
    const auto step = static_cast<std::uint32_t>(
        static_cast<float>(1U << 8U) * (kLfoFrequency[frequency] * 256.0F / 44100.0F));
    slot.pitchLfoStep = slot.pitchLfoScale != 0 ? step : 0;
    slot.amplitudeLfoStep = slot.amplitudeLfoScale != 0 ? step : 0;
}
void ScspSynth::executeKeyOn() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        if (word(i * 0x20) & 0x800) {
            if (slot.envelope == Envelope::Release) {
                refreshSlot(i);
                slot.active = true;
                slot.backwards = slot.loopEntered = false;
                slot.position = 0;
                slot.level = 383; // SCSP attack initial level used by MAME.
                slot.envelope = Envelope::Attack;
                // LFORE resets the LFO phase at key-on; otherwise it keeps
                // running, matching a free-running hardware oscillator.
                if ((word(i * 0x20 + 0x12) & 0x8000) != 0U) {
                    slot.pitchLfoPhase = 0;
                    slot.amplitudeLfoPhase = 0;
                }
                ++stats_.keyOns;
            }
        } else if (slot.active && slot.envelope != Envelope::Release) {
            slot.envelope = Envelope::Release;
            ++stats_.keyOffs;
        }
    }
}
void ScspSynth::stop(std::size_t index) {
    slots_[index].active = false;
    slots_[index].envelope = Envelope::Release;
    registers_[index * 0x20] &= static_cast<std::uint8_t>(~0x08U);
}
float ScspSynth::envelopeGain(std::size_t index) {
    auto& slot = slots_[index];
    const auto base = index * 0x20;
    const auto state = slot.envelope;
    if (state == Envelope::Attack) {
        slot.level = std::min(1023.0, slot.level + slot.rates[0]);
        if (slot.level >= 1023 && !(word(base + 10) & 0x4000)) slot.envelope = Envelope::Decay1;
        return (word(base + 8) & 0x20) ? 1.0F : static_cast<float>(slot.level / 1023.0);
    }
    slot.level = std::max(0.0, slot.level - slot.rates[static_cast<unsigned>(state)]);
    if (state == Envelope::Decay1 && static_cast<unsigned>(slot.level) / 32 <= 31U - ((word(base + 10) >> 5) & 31))
        slot.envelope = Envelope::Decay2;
    if (state == Envelope::Release && slot.level == 0) stop(index);
    return dbGain(static_cast<float>((1023.0 - slot.level) * 3.0 / 32.0));
}
float ScspSynth::pcm(std::size_t index, std::int64_t sample, std::span<std::uint8_t> ram) {
    const auto base = index * 0x20;
    const auto control = word(base);
    const bool pcm8 = (control & 0x10) != 0;
    const auto start = (static_cast<std::uint32_t>(control & 15) << 16) | word(base + 2);
    const auto address = ((pcm8 ? start : start & ~1U)
        + static_cast<std::uint32_t>(sample) * (pcm8 ? 1U : 2U)) & 0x7ffffU;
    const auto next = (address + 1) & 0x7ffffU;
    if (address >= ram.size() || (!pcm8 && next >= ram.size())) {
        ++stats_.outOfRangeSamples;
        return 0;
    }
    auto bits = static_cast<std::uint16_t>(ram[address] << 8);
    if (!pcm8) bits |= ram[next];
    if (control & 0x200) bits ^= 0x7fff;
    if (control & 0x400) bits ^= 0x8000;
    const auto signedSample = bits < 0x8000 ? static_cast<int>(bits) : static_cast<int>(bits) - 65536;
    return static_cast<float>(signedSample) / 32768.0F;
}
std::int32_t ScspSynth::nextNoise() noexcept {
    // The Sega manual does not document the LFSR taps, and MAME uses the host
    // RNG as an approximation. This deterministic 16-bit xorshift keeps the
    // output reproducible across runs while still producing white noise.
    noiseState_ = static_cast<std::uint16_t>(noiseState_ ^ (noiseState_ << 7U));
    noiseState_ = static_cast<std::uint16_t>(noiseState_ ^ (noiseState_ >> 9U));
    noiseState_ = static_cast<std::uint16_t>(noiseState_ ^ (noiseState_ << 8U));
    if (noiseState_ == 0U) noiseState_ = 0x4A3FU;
    return static_cast<std::int16_t>(noiseState_);
}
void ScspSynth::advancePosition(std::size_t index, std::uint32_t step) {
    auto& slot = slots_[index];
    const auto base = index * 0x20;
    const auto start = static_cast<std::int64_t>(word(base + 4)) * kOne;
    const auto end = static_cast<std::int64_t>(word(base + 6)) * kOne;
    const auto loop = (word(base) >> 5) & 3;
    slot.position += slot.backwards ? -static_cast<std::int64_t>(step) : step;
    if (!slot.loopEntered && slot.position >= start) {
        slot.loopEntered = true;
        if (slot.envelope == Envelope::Attack && (word(base + 10) & 0x4000))
            slot.envelope = Envelope::Decay1;
        if (loop == 2) {
            slot.backwards = true;
            slot.position = end - (slot.position - start);
        }
    }
    if (!loop) {
        if (slot.position >= end) stop(index);
    } else if (end <= start) {
        ++stats_.invalidLoops;
        stop(index);
    } else if (loop == 1 && slot.position >= end) {
        slot.position = start + positiveMod(slot.position - end, end - start);
    } else if (loop == 2 && slot.backwards && slot.position < start) {
        slot.position = end - positiveMod(start - slot.position, end - start);
    } else if (loop == 3 && slot.loopEntered) {
        const auto length = end - start;
        const auto phase = positiveMod(slot.backwards ? 2 * length - (slot.position - start)
                                                     : slot.position - start, 2 * length);
        slot.backwards = phase >= length;
        slot.position = start + (slot.backwards ? 2 * length - phase : phase);
    }
}
std::array<float, 2> ScspSynth::renderSample(std::span<std::uint8_t> ram) {
    const auto& tables = lfoTables();
    soundRam_ = ram;
    if (dmaPending_) {
        dmaPending_ = false;
        executeDma();
    }
    std::array<float, 2> output{};
    ++stats_.renderedSamples;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        const auto base = i * 0x20;
        const auto control = word(base);
        const auto source = (control >> 7) & 3;
        if (slot.active) {
            const auto end = word(base + 6);
            const auto start = word(base + 4);
            const auto loop = (control >> 5) & 3;
            if (!end || (loop && end <= start)) {
                if (loop) ++stats_.invalidLoops;
                stop(i);
            } else {
                auto step = slot.step;
                if (slot.pitchLfoScale != 0U) {
                    slot.pitchLfoPhase = (slot.pitchLfoPhase + slot.pitchLfoStep) & 0xFFFFU;
                    const auto index = static_cast<std::size_t>((slot.pitchLfoPhase >> 8U) & 0xFFU);
                    const auto waveform = tables.pitchWaveforms[slot.pitchLfoShape][index];
                    const auto ratio = tables.pitchScales[slot.pitchLfoScale]
                        [static_cast<std::size_t>(waveform + 128)];
                    step = (slot.step * ratio) >> 8U;
                    ++stats_.pitchLfoSamples;
                }
                auto readPosition = slot.position;
                if (slot.modulationLevel != 0U) {
                    const auto mask = kRingBufferSize - 1U;
                    const auto x = ringBuffer_[(ringPointer_ + slot.modulationX) & mask];
                    const auto y = ringBuffer_[(ringPointer_ + slot.modulationY) & mask];
                    std::int32_t modulation = (x + y) / 2;
                    modulation <<= 10;
                    modulation >>= 26 - static_cast<int>(slot.modulationLevel);
                    if (!slot.pcm8) modulation <<= 1;
                    readPosition += static_cast<std::int64_t>(modulation) * kOne;
                    ++stats_.fmSamples;
                }
                const auto sampleIndex = readPosition / kOne;
                auto nextIndex = sampleIndex + 1;
                if (loop == 1 && nextIndex >= end) nextIndex = start;
                else if (nextIndex > end || (!loop && nextIndex >= end)) nextIndex = sampleIndex;
                const auto fraction = static_cast<float>(positiveMod(readPosition, kOne))
                    / static_cast<float>(kOne);
                float sample = 0.0F;
                if (source == 0) {
                    const auto first = pcm(i, sampleIndex, ram);
                    sample = fraction == 0 ? first
                        : first + (pcm(i, nextIndex, ram) - first) * fraction;
                } else if (source == 1) {
                    auto bits = static_cast<std::uint16_t>(nextNoise());
                    if (control & 0x200) bits ^= 0x7fff;
                    if (control & 0x400) bits ^= 0x8000;
                    sample = static_cast<float>(static_cast<std::int16_t>(bits)) / 32768.0F;
                    ++stats_.noiseSamples;
                } else if (source == 3) {
                    // The manual marks SSCTL=3 as unusable; it stays silent.
                    ++stats_.unsupportedSourceSamples;
                }
                const auto envelope = (word(base + 0x0c) & 0x100) ? 1.0F : envelopeGain(i);
                auto amplitude = 1.0F;
                if (slot.amplitudeLfoScale != 0U) {
                    slot.amplitudeLfoPhase = (slot.amplitudeLfoPhase + slot.amplitudeLfoStep) & 0xFFFFU;
                    const auto index = static_cast<std::size_t>((slot.amplitudeLfoPhase >> 8U) & 0xFFU);
                    const auto waveform = tables.amplitudeWaveforms[slot.amplitudeLfoShape][index];
                    amplitude = static_cast<float>(
                        tables.amplitudeScales[slot.amplitudeLfoScale][waveform]) / 256.0F;
                    ++stats_.amplitudeLfoSamples;
                }
                sample *= envelope * amplitude;
                const auto sampleInt = static_cast<std::int32_t>(
                    std::lround(static_cast<double>(sample) * 32768.0));
                if (slot.imxl != 0U && slot.isel < ScspDsp::kMixCount && !dsp_.stopped()) {
                    // A stopped DSP ignores the sends; accumulating them would
                    // only overflow MIXS until the program is loaded.
                    dsp_.mixSample(slot.isel, static_cast<std::int32_t>(std::lround(
                        static_cast<double>(sampleInt) * 16.0 * slot.sendGain)));
                    ++stats_.dspSendSamples;
                }
                if (slot.ringWrite) {
                    ringBuffer_[ringPointer_ & (kRingBufferSize - 1U)] =
                        static_cast<std::int32_t>(std::lround(
                            static_cast<double>(sampleInt) * 2.0 * slot.totalLevelGain));
                }
                output[0] += sample * slot.gain[0];
                output[1] += sample * slot.gain[1];
                if (slot.active) advancePosition(i, step);
            }
        }
        // The modulation ring buffer advances once per slot, as on hardware.
        ++ringPointer_;
    }
    if (!dsp_.stopped()) dsp_.step(ram.data(), ram.data(), ram.size());
    const auto& effect = dsp_.effectRegisters();
    for (std::size_t i = 0; i < ScspDsp::kEffectCount && i < slots_.size(); ++i) {
        const auto sendLevel = slots_[i].effectSendLevel;
        if (sendLevel == 0U) continue;
        const auto level = dbGain(static_cast<float>((7U - sendLevel) * 6U));
        const auto pan = slots_[i].effectPan;
        const auto singlePan = panGain(pan);
        const auto value = static_cast<float>(effect[i]) / 32768.0F * level;
        output[0] += value * (pan < 16U ? singlePan : 1.0F);
        output[1] += value * (pan < 16U ? 1.0F : singlePan);
        ++stats_.dspEffectSamples;
    }
    const auto master = static_cast<float>(word(0x400) & 15) / 15.0F;
    for (auto& channel : output) channel = std::clamp(channel * master, -1.0F, 1.0F);
    std::uint32_t sounding{};
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].active) sounding |= 1U << i;
    stats_.activeSlotMask = sounding;
    const auto selected = (word(0x408) >> 11) & 31;
    const auto& slot = slots_[selected];
    const auto address = static_cast<std::uint16_t>((slot.position >> 24) & 15);
    const auto attenuation = static_cast<std::uint16_t>(31 - (static_cast<unsigned>(slot.level) >> 5));
    monitor_ = static_cast<std::uint16_t>((address << 7) | (static_cast<unsigned>(slot.envelope) << 5) | attenuation);
    return output;
}
std::size_t ScspSynth::activeVoiceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.active; }));
}
void ScspSynth::setDmaCompletionHook(std::function<void()> hook) {
    dmaCompletion_ = std::move(hook);
}
std::uint32_t ScspSynth::ramWordAt(std::uint32_t address) const noexcept {
    if (address + 2U > soundRam_.size()) return 0U;
    return (static_cast<std::uint32_t>(soundRam_[address]) << 8U) |
           static_cast<std::uint32_t>(soundRam_[address + 1U]);
}
void ScspSynth::requestDma() {
    // Before the first rendered sample the RAM image is unknown; defer the
    // transfer so the guest still sees it complete.
    if (soundRam_.empty()) {
        dmaPending_ = true;
        return;
    }
    executeDma();
}
void ScspSynth::executeDma() {
    if (dmaActive_) return; // A register write inside the transfer must not recurse.
    dmaActive_ = true;
    const auto length = dmaLength_ & 0x0FFEU;
    auto memory = dmaMemory_ & 0x7FFFEU;
    auto registerOffset = dmaRegister_ & 0x0FFEU;
    for (std::uint32_t byte = 0; byte < length; byte += 2U) {
        if (dmaGate_) {
            // DGATE clears the destination instead of copying.
            if (dmaDirection_) {
                if (memory + 2U <= soundRam_.size()) {
                    soundRam_[memory] = 0U;
                    soundRam_[memory + 1U] = 0U;
                    ++stats_.dmaWords;
                } else {
                    ++stats_.dmaDropped;
                }
            } else {
                static_cast<void>(write(registerOffset, 2U, 0U));
            }
        } else if (dmaDirection_) {
            const auto value = read(registerOffset, 2U).value_or(0U);
            if (memory + 2U <= soundRam_.size()) {
                soundRam_[memory] = static_cast<std::uint8_t>(value >> 8U);
                soundRam_[memory + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
                ++stats_.dmaWords;
            } else {
                ++stats_.dmaDropped;
            }
        } else {
            static_cast<void>(write(registerOffset, 2U, ramWordAt(memory)));
        }
        memory = (memory + 2U) & 0x7FFFEU;
        registerOffset = (registerOffset + 2U) & 0x0FFEU;
    }
    registers_[0x416] &= static_cast<std::uint8_t>(~0x10U); // Clear DEXE.
    ++stats_.dmaTransfers;
    dmaActive_ = false;
    if (dmaCompletion_) dmaCompletion_();
}
} // namespace saturn::platform
