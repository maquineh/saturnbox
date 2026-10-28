#include "saturn/saturn/scsp_timing.hpp"

#include <algorithm>

namespace saturn::platform {
namespace {

constexpr std::uint16_t kPendingMask = 0x07FFU;
constexpr std::uint16_t kManualInterrupt = 1U << 5U;
constexpr std::array<std::uint16_t, 3> kTimerPendingBits{
    1U << 6U, 1U << 7U, 1U << 8U};
constexpr std::array<std::uint16_t, 3> kTimerOffsets{
    ScspTiming::kTimerA, ScspTiming::kTimerB, ScspTiming::kTimerC};
constexpr std::array<std::uint16_t, 3> kLevelOffsets{
    ScspTiming::kScilv0, ScspTiming::kScilv1, ScspTiming::kScilv2};

[[nodiscard]] bool supportedWidth(std::uint8_t width) noexcept {
    return width == 1U || width == 2U || width == 4U;
}

} // namespace

std::optional<std::uint32_t> ScspTiming::read(std::uint16_t offset,
                                              std::uint8_t width) const {
    if (!supportedWidth(width) ||
        static_cast<std::uint32_t>(offset) + width - 1U > 0xFFFFU) {
        return std::nullopt;
    }
    for (std::uint8_t index = 0; index < width; ++index) {
        if (!isMapped(static_cast<std::uint16_t>(offset + index))) {
            return std::nullopt;
        }
    }
    std::uint32_t value{};
    for (std::uint8_t index = 0; index < width; ++index) {
        value = (value << 8U) | readByte(static_cast<std::uint16_t>(offset + index));
    }
    return value;
}

bool ScspTiming::write(std::uint16_t offset, std::uint8_t width,
                       std::uint32_t value) {
    if (!supportedWidth(width) ||
        static_cast<std::uint32_t>(offset) + width - 1U > 0xFFFFU) {
        return false;
    }
    for (std::uint8_t index = 0; index < width; ++index) {
        if (!isMapped(static_cast<std::uint16_t>(offset + index))) {
            return false;
        }
    }
    for (std::uint8_t index = 0; index < width; ++index) {
        const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
        writeByte(static_cast<std::uint16_t>(offset + index),
                  static_cast<std::uint8_t>(value >> shift));
    }
    return true;
}

void ScspTiming::advanceSamples(std::uint64_t count) noexcept {
    stats_.samplesAdvanced += count;
    for (std::size_t index = 0; index < stats_.timers.size(); ++index) {
        advanceTimer(index, count);
    }
}

std::uint8_t ScspTiming::irqLevel() const noexcept {
    std::uint8_t highest{};
    const auto active = static_cast<std::uint16_t>(
        stats_.interruptEnable & stats_.interruptPending & kPendingMask);
    for (std::uint8_t factor = 0; factor <= 10U; ++factor) {
        if ((active & (1U << factor)) == 0U) continue;
        const auto column = factor < 7U ? factor : 7U;
        const auto level = static_cast<std::uint8_t>(
            ((interruptLevels_[0] >> column) & 1U) |
            (((interruptLevels_[1] >> column) & 1U) << 1U) |
            (((interruptLevels_[2] >> column) & 1U) << 2U));
        highest = std::max(highest, level);
    }
    return highest;
}

const ScspTiming::Stats& ScspTiming::stats() const noexcept { return stats_; }

bool ScspTiming::isMapped(std::uint16_t offset) noexcept {
    return offset >= kTimerA && offset <= kScilv2 + 1U;
}

std::uint8_t ScspTiming::readByte(std::uint16_t offset) const noexcept {
    // Timer registers, SCIRE, and SCILV are write-only in the Sega register
    // map, so reads return zero. Timer state remains available in Stats.
    if (offset >= kTimerA && offset <= kTimerC + 1U) return 0U;
    if (offset >= kScire && offset <= kScire + 1U) return 0U;
    if (offset >= kScilv0 && offset <= kScilv2 + 1U) return 0U;

    if (offset >= kScieb && offset <= kScieb + 1U) {
        const auto shift = (offset & 1U) == 0U ? 8U : 0U;
        return static_cast<std::uint8_t>(stats_.interruptEnable >> shift);
    }
    if (offset >= kScipd && offset <= kScipd + 1U) {
        const auto shift = (offset & 1U) == 0U ? 8U : 0U;
        return static_cast<std::uint8_t>(stats_.interruptPending >> shift);
    }
    return 0U;
}

void ScspTiming::writeByte(std::uint16_t offset, std::uint8_t value) noexcept {
    for (std::size_t index = 0; index < kTimerOffsets.size(); ++index) {
        const auto base = kTimerOffsets[index];
        auto& timer = stats_.timers[index];
        if (offset == base) {
            timer.prescaler = static_cast<std::uint8_t>(value & 7U);
            timer.sampleRemainder = 0U;
            timer.running = timer.count != 0xFFU;
            return;
        }
        if (offset == base + 1U) {
            timer.count = value;
            timer.sampleRemainder = 0U;
            timer.running = timer.count != 0xFFU;
            return;
        }
    }

    if (offset == kScieb || offset == kScieb + 1U) {
        const auto shift = (offset & 1U) == 0U ? 8U : 0U;
        const auto mask = static_cast<std::uint16_t>(0xFFU << shift);
        stats_.interruptEnable = static_cast<std::uint16_t>(
            ((stats_.interruptEnable & ~mask) |
             (static_cast<std::uint16_t>(value) << shift)) & kPendingMask);
    } else if (offset == kScipd + 1U) {
        if ((value & (1U << 5U)) != 0U) setPending(kManualInterrupt);
    } else if (offset == kScire || offset == kScire + 1U) {
        const auto shift = (offset & 1U) == 0U ? 8U : 0U;
        const auto clearMask = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(value) << shift) & kPendingMask);
        stats_.interruptPending = static_cast<std::uint16_t>(
            stats_.interruptPending & ~clearMask);
    } else {
        for (std::size_t index = 0; index < kLevelOffsets.size(); ++index) {
            if (offset == kLevelOffsets[index] + 1U) {
                interruptLevels_[index] = value;
                return;
            }
        }
    }
}

void ScspTiming::advanceTimer(std::size_t index,
                              std::uint64_t sampleCount) noexcept {
    auto& timer = stats_.timers[index];
    if (!timer.running) return;
    const auto divisor = std::uint64_t{1U} << timer.prescaler;
    const auto wholeTicks = sampleCount / divisor;
    const auto sampleRemainder = sampleCount % divisor;
    const auto accumulated = static_cast<std::uint64_t>(timer.sampleRemainder) +
                             sampleRemainder;
    const auto ticks = wholeTicks + accumulated / divisor;
    timer.sampleRemainder = static_cast<std::uint8_t>(accumulated % divisor);
    if (ticks == 0U) return;

    // MAME models timer callbacks as one-shot: the counter is set to FFh and
    // stopped until a timer-register write reloads it. The Sega manual says
    // an interrupt occurs at FFh but does not specify wrap/reload behavior.
    if (ticks >= static_cast<std::uint64_t>(0xFFU - timer.count)) {
        timer.count = 0xFFU;
        timer.sampleRemainder = 0U;
        timer.running = false;
        ++timer.expirations;
        setPending(kTimerPendingBits[index]);
        return;
    }
    timer.count = static_cast<std::uint8_t>(
        static_cast<std::uint64_t>(timer.count) + ticks);
}

void ScspTiming::setPending(std::uint16_t mask) noexcept {
    stats_.interruptPending = static_cast<std::uint16_t>(
        (stats_.interruptPending | mask) & kPendingMask);
}

} // namespace saturn::platform
