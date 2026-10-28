// SCSP effect DSP ported from MAME's scspdsp.cpp.
// Copyright ElSemi, R. Belmont. BSD-3-Clause; see
// third_party/MAME-SCSP-LICENSE.txt.
#include "saturn/saturn/scsp_dsp.hpp"

#include <algorithm>

namespace saturn::platform {
namespace {
// MAME keeps 24-bit values in signed 32-bit words and relies on two's
// complement wrapping; these helpers make that explicit instead of UB.
[[nodiscard]] constexpr std::int32_t sext(std::int32_t value, unsigned bits) noexcept {
    const std::uint32_t mask = (1U << bits) - 1U;
    const std::uint32_t bitsValue = static_cast<std::uint32_t>(value) & mask;
    const std::uint32_t signBit = 1U << (bits - 1U);
    return static_cast<std::int32_t>((bitsValue & signBit) != 0U
        ? static_cast<std::int64_t>(bitsValue) - (static_cast<std::int64_t>(1) << bits)
        : static_cast<std::int64_t>(bitsValue));
}

// Compresses a 24-bit accumulator value into the DSP's 1.11 floating format.
[[nodiscard]] std::uint16_t pack(std::int32_t value) noexcept {
    const auto sign = (static_cast<std::uint32_t>(value) >> 23) & 1U;
    std::uint32_t temp = (static_cast<std::uint32_t>(value) ^
        (static_cast<std::uint32_t>(value) << 1U)) & 0xFFFFFFU;
    int exponent = 0;
    for (int step = 0; step < 12; ++step) {
        if ((temp & 0x800000U) != 0U) break;
        temp <<= 1U;
        ++exponent;
    }
    std::uint32_t packed = static_cast<std::uint32_t>(value);
    if (exponent < 12) packed = (packed << exponent) & 0x3FFFFFU;
    else packed <<= 11U;
    packed >>= 11U;
    packed &= 0x7FFU;
    packed |= sign << 15U;
    packed |= static_cast<std::uint32_t>(exponent) << 11U;
    return static_cast<std::uint16_t>(packed);
}

[[nodiscard]] std::int32_t unpack(std::uint16_t value) noexcept {
    const auto sign = (value >> 15) & 1U;
    int exponent = (value >> 11) & 0xF;
    const auto mantissa = value & 0x7FFU;
    std::uint32_t assembled = static_cast<std::uint32_t>(mantissa) << 11U;
    if (exponent > 11) {
        exponent = 11;
        assembled |= static_cast<std::uint32_t>(sign) << 22U;
    } else {
        assembled |= (sign ^ 1U) << 22U;
    }
    assembled |= static_cast<std::uint32_t>(sign) << 23U;
    return sext(static_cast<std::int32_t>(assembled), 24) >> exponent;
}
} // namespace

ScspDsp::ScspDsp() { reset(); }

void ScspDsp::reset() {
    coefficients_.fill(0);
    addresses_.fill(0);
    program_.fill(0);
    temp_.fill(0);
    memory_.fill(0);
    mixRegisters_.fill(0);
    external_.fill(0);
    effectRegisters_.fill(0);
    ringBase_ = 0;
    ringLength_ = 8U * 1024U;
    dec_ = 0;
    stopped_ = true;
    lastStep_ = 0;
    stats_ = {};
}

void ScspDsp::start() {
    stopped_ = false;
    auto step = static_cast<int>(kStepCount) - 1;
    for (; step >= 0; --step) {
        const auto* instruction = &program_[static_cast<std::size_t>(step) * 4U];
        if (instruction[0] != 0U || instruction[1] != 0U ||
            instruction[2] != 0U || instruction[3] != 0U) {
            break;
        }
    }
    lastStep_ = static_cast<std::uint8_t>(step + 1);
}

void ScspDsp::setRingBuffer(std::uint16_t base, std::uint16_t length) noexcept {
    ringBase_ = base;
    ringLength_ = length == 0U ? 1U : length;
}

void ScspDsp::mixSample(std::size_t index, std::int32_t sample) noexcept {
    if (index < mixRegisters_.size()) mixRegisters_[index] += sample;
}

std::int32_t ScspDsp::mixRegister(std::size_t index) const noexcept {
    return index < mixRegisters_.size() ? mixRegisters_[index] : 0;
}

void ScspDsp::registerWord(std::uint16_t offset, std::uint16_t value) noexcept {
    if (offset < 0x700U || offset >= 0xEE4U) return;
    if (offset < 0x780U) {
        coefficients_[(offset - 0x700U) / 2U] = value;
    } else if (offset < 0x7C0U) {
        addresses_[(offset - 0x780U) / 2U] = value;
    } else if (offset < 0x800U) {
        // 0x7C0-0x7FF mirrors MADRS on reads only; writes are ignored. Real
        // programs clear this range right after setting MADRS.
    } else if (offset < 0xC00U) {
        program_[(offset - 0x800U) / 2U] = value;
        if (offset == 0xBF0U) start();
    }
}

std::uint16_t ScspDsp::readWord(std::uint16_t offset) const noexcept {
    if (offset < 0x700U) return 0;
    if (offset < 0x780U) return coefficients_[(offset - 0x700U) / 2U];
    if (offset < 0x7C0U) return addresses_[(offset - 0x780U) / 2U];
    if (offset < 0x800U) return addresses_[(offset - 0x7C0U) / 2U];
    if (offset < 0xC00U) return program_[(offset - 0x800U) / 2U];
    const auto lane = [offset](std::int32_t value) -> std::uint16_t {
        return (offset & 2U) != 0U ? static_cast<std::uint16_t>(value & 0xFFFF)
                                   : static_cast<std::uint16_t>(
                                         static_cast<std::uint32_t>(value) >> 16U);
    };
    if (offset < 0xE00U) return lane(temp_[(offset >> 2U) & 0x7FU]);
    if (offset < 0xE80U) return lane(memory_[(offset >> 2U) & 0x1FU]);
    if (offset < 0xEC0U) return lane(mixRegisters_[(offset >> 2U) & 0xFU]);
    if (offset < 0xEE0U) return static_cast<std::uint16_t>(
        effectRegisters_[(offset - 0xEC0U) / 2U] & 0xFFFF);
    if (offset < 0xEE4U) return static_cast<std::uint16_t>(external_[(offset - 0xEE0U) / 2U]);
    return 0;
}

bool ScspDsp::isReadableRegister(std::uint16_t offset) noexcept {
    // TEMP (0xC00), MEMS (0xE00), MIXS (0xE80), EFREG (0xEC0) and EXTS (0xEE0).
    return offset >= 0xC00U && offset < 0xEE4U;
}

std::optional<std::uint8_t> ScspDsp::readByte(std::uint16_t offset) const noexcept {
    if (!isReadableRegister(offset)) return std::nullopt;
    const auto word = readWord(static_cast<std::uint16_t>(offset & ~1U));
    return static_cast<std::uint8_t>((offset & 1U) != 0U ? (word & 0xFFU) : (word >> 8U));
}

std::int32_t ScspDsp::ramWord(const std::uint8_t* ram, std::size_t word) const noexcept {
    if (ram == nullptr || word >= ramWordLimit_) return 0;
    const auto base = word * 2U;
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(ram[base]) << 8U) |
                                     static_cast<std::uint32_t>(ram[base + 1U]));
}

bool ScspDsp::writeRamWord(std::uint8_t* ramOut, std::size_t word,
                           std::uint16_t value) noexcept {
    if (ramOut == nullptr || word >= ramWordLimit_) {
        ++stats_.droppedWrites;
        return false;
    }
    const auto base = word * 2U;
    ramOut[base] = static_cast<std::uint8_t>(value >> 8U);
    ramOut[base + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
    return true;
}

void ScspDsp::step(const std::uint8_t* ram, std::uint8_t* ramOut, std::size_t ramBytes) {
    if (stopped_) return;
    ramWordLimit_ = std::min<std::size_t>(ramBytes / 2U, kRamWords);
    effectRegisters_.fill(0);
    ++stats_.samples;

    std::int32_t accumulator = 0;
    std::int32_t memoryValue = 0;
    std::int32_t fraction = 0;
    std::int32_t yRegister = 0;
    std::uint32_t addressRegister = 0;

    for (std::size_t index = 0; index < lastStep_; ++index) {
        const auto* instruction = &program_[index * 4U];
        const std::uint32_t tra = (instruction[0] >> 8U) & 0x7FU;
        const std::uint32_t twt = (instruction[0] >> 7U) & 0x01U;
        const std::uint32_t twa = instruction[0] & 0x7FU;

        const std::uint32_t xsel = (instruction[1] >> 15U) & 0x01U;
        const std::uint32_t ysel = (instruction[1] >> 13U) & 0x03U;
        const std::uint32_t ira = (instruction[1] >> 6U) & 0x3FU;
        const std::uint32_t iwt = (instruction[1] >> 5U) & 0x01U;
        const std::uint32_t iwa = instruction[1] & 0x1FU;

        const std::uint32_t table = (instruction[2] >> 15U) & 0x01U;
        const std::uint32_t mwt = (instruction[2] >> 14U) & 0x01U;
        const std::uint32_t mrd = (instruction[2] >> 13U) & 0x01U;
        const std::uint32_t ewt = (instruction[2] >> 12U) & 0x01U;
        const std::uint32_t ewa = (instruction[2] >> 8U) & 0x0FU;
        const std::uint32_t adrl = (instruction[2] >> 7U) & 0x01U;
        const std::uint32_t frcl = (instruction[2] >> 6U) & 0x01U;
        const std::uint32_t shift = (instruction[2] >> 4U) & 0x03U;
        const std::uint32_t yrl = (instruction[2] >> 3U) & 0x01U;
        const std::uint32_t negb = (instruction[2] >> 2U) & 0x01U;
        const std::uint32_t zero = (instruction[2] >> 1U) & 0x01U;
        const std::uint32_t bsel = instruction[2] & 0x01U;

        const std::uint32_t coef = (instruction[3] >> 9U) & 0x3FU;
        const std::uint32_t masa = (instruction[3] >> 2U) & 0x1FU;
        const std::uint32_t adreb = (instruction[3] >> 1U) & 0x01U;
        const std::uint32_t nxadr = instruction[3] & 0x01U;

        std::int32_t inputs = 0;
        if (ira <= 0x1FU) inputs = memory_[ira];
        else if (ira <= 0x2FU) inputs = mixRegisters_[ira - 0x20U] << 4U;
        else if (ira <= 0x31U) inputs = external_[ira - 0x30U] << 8U;
        else return;
        inputs = sext(inputs, 24);

        if (iwt != 0U) {
            memory_[iwa] = memoryValue;
            if (ira == iwa) inputs = memoryValue;
        }

        std::int32_t b = 0;
        if (zero == 0U) {
            b = bsel != 0U ? accumulator
                           : sext(temp_[static_cast<std::size_t>((tra + static_cast<std::uint32_t>(dec_)) & 0x7FU)], 24);
            if (negb != 0U) b = -b;
        }

        const auto tempValue = sext(temp_[static_cast<std::size_t>((tra + static_cast<std::uint32_t>(dec_)) & 0x7FU)], 24);
        std::int32_t x = xsel != 0U ? inputs : tempValue;

        std::int32_t y = 0;
        if (ysel == 0U) y = fraction;
        else if (ysel == 1U) y = static_cast<std::int32_t>(coefficients_[coef] >> 3U);
        else if (ysel == 2U) y = (yRegister >> 11U) & 0x1FFF;
        else y = (yRegister >> 4U) & 0x0FFFU;

        if (yrl != 0U) yRegister = inputs;

        const auto doubled = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(accumulator) << 1U);
        std::int32_t shifted = 0;
        if (shift == 0U) shifted = std::clamp<std::int32_t>(accumulator, -0x00800000, 0x007FFFFF);
        else if (shift == 1U) {
            shifted = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(accumulator) * 2, -0x00800000, 0x007FFFFF));
        } else if (shift == 2U) shifted = sext(doubled, 24);
        else shifted = sext(accumulator, 24);

        y = sext(y, 13);
        const auto product = (static_cast<std::int64_t>(x) * static_cast<std::int64_t>(y)) >> 12;
        accumulator = static_cast<std::int32_t>(product + b);

        if (twt != 0U) {
            temp_[static_cast<std::size_t>((twa + static_cast<std::uint32_t>(dec_)) & 0x7FU)] = shifted;
        }
        if (frcl != 0U) {
            fraction = shift == 3U ? (shifted & 0x0FFF) : ((shifted >> 11U) & 0x1FFF);
        }
        if (mrd != 0U || mwt != 0U) {
            auto address = static_cast<std::uint32_t>(addresses_[masa]);
            if (table == 0U) address += static_cast<std::uint32_t>(dec_);
            if (adreb != 0U) address += addressRegister & 0x0FFFU;
            if (nxadr != 0U) ++address;
            if (table == 0U) address &= ringLength_ - 1U;
            else address &= 0xFFFFU;
            address += static_cast<std::uint32_t>(ringBase_) << 12U;
            if (mrd != 0U && (index & 1U) != 0U) {
                const auto word = static_cast<std::uint16_t>(ramWord(ram, address));
                memoryValue = ((instruction[3] >> 15U) & 1U) != 0U
                    ? static_cast<std::int32_t>(word) << 8U
                    : unpack(word);
                ++stats_.reads;
            }
            if (mwt != 0U && (index & 1U) != 0U) {
                if (writeRamWord(ramOut, address,
                        ((instruction[3] >> 15U) & 1U) != 0U
                            ? static_cast<std::uint16_t>(static_cast<std::uint32_t>(shifted) >> 8U)
                            : pack(shifted))) {
                    ++stats_.writes;
                }
            }
        }
        if (adrl != 0U) {
            addressRegister = shift == 3U
                ? static_cast<std::uint32_t>((shifted >> 12U) & 0x0FFFU)
                : static_cast<std::uint32_t>(inputs >> 16U);
        }
        if (ewt != 0U) effectRegisters_[ewa] += shifted >> 8U;
        ++stats_.steps;
    }

    --dec_;
    mixRegisters_.fill(0);
}

} // namespace saturn::platform
