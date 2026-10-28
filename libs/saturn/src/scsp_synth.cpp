// Envelope timing tables and amplitude/pan equations adapted from MAME SCSP.
// Copyright ElSemi, R. Belmont. BSD-3-Clause; see third_party/MAME-SCSP-LICENSE.txt.
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
}

ScspSynth::ScspSynth() { reset(); }
void ScspSynth::reset() {
    registers_.fill(0);
    slots_.fill(Slot{});
    monitor_ = 0x7f; // Release at minimum level.
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
    }
    return true;
}
std::optional<std::uint32_t> ScspSynth::read(std::uint32_t offset, std::uint8_t width) const {
    if (!validAccess(offset, width)) return std::nullopt;
    std::uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
        const auto address = offset + i;
        const auto byte = address == 0x408 ? static_cast<std::uint8_t>(monitor_ >> 8)
            : address == 0x409 ? static_cast<std::uint8_t>(monitor_) : registers_[address];
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
    const auto mix = word(base + 0x16);
    const auto send = (mix >> 13) & 7;
    const auto pan = (mix >> 8) & 31;
    const auto gain = send ? dbGain(attenuation + static_cast<float>((7 - send) * 6)) : 0.0F;
    const auto panGain = (pan & 15) == 15 ? 0.0F : dbGain(static_cast<float>((pan & 15) * 3));
    slot.gain = pan < 16 ? std::array<float, 2>{gain * panGain, gain}
                        : std::array<float, 2>{gain, gain * panGain};
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
float ScspSynth::pcm(std::size_t index, std::int64_t sample, std::span<const std::uint8_t> ram) {
    const auto base = index * 0x20;
    const auto control = word(base);
    const auto source = (control >> 7) & 3;
    if (source) return 0; // SSCTL=2 is specified silence; noise/3 are unsupported.
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
void ScspSynth::advancePosition(std::size_t index) {
    auto& slot = slots_[index];
    const auto base = index * 0x20;
    const auto start = static_cast<std::int64_t>(word(base + 4)) * kOne;
    const auto end = static_cast<std::int64_t>(word(base + 6)) * kOne;
    const auto loop = (word(base) >> 5) & 3;
    slot.position += slot.backwards ? -static_cast<std::int64_t>(slot.step) : slot.step;
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
std::array<float, 2> ScspSynth::renderSample(std::span<const std::uint8_t> ram) {
    std::array<float, 2> output{};
    ++stats_.renderedSamples;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        if (!slot.active) continue;
        const auto base = i * 0x20;
        const auto control = word(base);
        const auto source = (control >> 7) & 3;
        stats_.unsupportedSourceSamples += source == 1 || source == 3;
        stats_.unsupportedLfoSamples += (word(base + 0x12) & 0xe7) != 0;
        stats_.unsupportedFmSamples += (word(base + 0x0e) & 0xf000) != 0;
        stats_.unsupportedDspSamples += (word(base + 0x14) & 7) != 0 || (word(base + 0x16) & 0xe0) != 0;
        const auto end = word(base + 6);
        const auto start = word(base + 4);
        const auto loop = (control >> 5) & 3;
        if (!end || (loop && end <= start)) {
            if (loop) ++stats_.invalidLoops;
            stop(i);
            continue;
        }
        const auto sampleIndex = slot.position / kOne;
        auto nextIndex = sampleIndex + 1;
        if (loop == 1 && nextIndex >= end) nextIndex = start;
        else if (nextIndex > end || (!loop && nextIndex >= end)) nextIndex = sampleIndex;
        const auto fraction = static_cast<float>(slot.position % kOne) / static_cast<float>(kOne);
        const auto first = pcm(i, sampleIndex, ram);
        const auto sample = fraction == 0 ? first : first + (pcm(i, nextIndex, ram) - first) * fraction;
        const auto envelope = (word(base + 0x0c) & 0x100) ? 1.0F : envelopeGain(i);
        output[0] += sample * envelope * slot.gain[0];
        output[1] += sample * envelope * slot.gain[1];
        if (slot.active) advancePosition(i);
    }
    const auto master = static_cast<float>(word(0x400) & 15) / 15.0F;
    for (auto& channel : output) channel = std::clamp(channel * master, -1.0F, 1.0F);
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
} // namespace saturn::platform
