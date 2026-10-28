#include "test_support.hpp"
#include "saturn/saturn/scsp_timing.hpp"

namespace {
using saturn::platform::ScspTiming;

void prescaledTimersLatchEvenWhenMasked() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kTimerA, 4, 0x03FE02FEU));
    timing.advanceSamples(3);
    REQUIRE(timing.stats().interruptPending == 0U);
    timing.advanceSamples(1);
    REQUIRE((timing.stats().interruptPending & 0xC0U) == 0x80U);
    REQUIRE(timing.irqLevel() == 0U); // Masked pending remains observable.
    timing.advanceSamples(4);
    REQUIRE((timing.stats().interruptPending & 0xC0U) == 0xC0U);
    REQUIRE(timing.stats().timers[0].count == 0xFFU);
    REQUIRE(timing.stats().timers[1].expirations == 1U);
    REQUIRE(timing.write(ScspTiming::kScilv1 + 1U, 1, 0x80U));
    REQUIRE(timing.write(ScspTiming::kScieb + 1U, 1, 0x80U));
    REQUIRE(timing.irqLevel() == 2U); // Enabling reveals existing Timer B.
}

void timerReloadRestartsThePrescaler() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kTimerB, 2, 0x07FEU));
    timing.advanceSamples(127);
    REQUIRE(timing.stats().timers[1].expirations == 0U);
    REQUIRE(timing.write(ScspTiming::kTimerB + 1U, 1, 0xFEU));
    timing.advanceSamples(127);
    REQUIRE(timing.stats().timers[1].expirations == 0U);
    timing.advanceSamples(1);
    REQUIRE(timing.stats().timers[1].expirations == 1U);
    timing.advanceSamples(1024);
    REQUIRE(timing.stats().timers[1].expirations == 1U);
    REQUIRE(timing.stats().timers[1].count == 0xFFU);
}

void manualIrqAndWriteOneClearUseBigEndianLanes() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kScilv0, 4, 0x00200020U));
    REQUIRE(timing.write(ScspTiming::kScieb, 2, 0x0020U));
    REQUIRE(timing.write(ScspTiming::kScipd + 1U, 1, 0x20U));
    REQUIRE(timing.irqLevel() == 3U);
    REQUIRE(timing.read(ScspTiming::kScipd, 2) == 0x20U);
    REQUIRE(timing.write(ScspTiming::kScipd, 2, 0U));
    REQUIRE(timing.irqLevel() == 3U); // Writing zero cannot clear pending.
    REQUIRE(timing.write(ScspTiming::kScire, 2, 0U));
    REQUIRE(timing.irqLevel() == 3U);
    REQUIRE(timing.write(ScspTiming::kScire + 1U, 1, 0x20U));
    REQUIRE(timing.irqLevel() == 0U);
    REQUIRE(!timing.write(0x417U, 2, 0xFFFFU));
    REQUIRE(!timing.read(0x429U, 2).has_value());
}

void timerCUsesSharedInterruptLevelColumn() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kScilv0 + 1U, 1, 0x80U));
    REQUIRE(timing.write(ScspTiming::kScilv2 + 1U, 1, 0x80U));
    REQUIRE(timing.write(ScspTiming::kScieb, 1, 1U)); // High byte: factor 8.
    REQUIRE(timing.write(ScspTiming::kTimerC, 2, 0x00FEU));
    timing.advanceSamples(1);
    REQUIRE(timing.irqLevel() == 5U);
    REQUIRE(timing.read(ScspTiming::kScipd, 1) == 1U);
}
} // namespace

void registerScspTimingTests(test::Registry& registry) {
    registry.add("SCSP: prescaled timers latch masked pending", prescaledTimersLatchEvenWhenMasked);
    registry.add("SCSP: timer reload restarts prescaler", timerReloadRestartsThePrescaler);
    registry.add("SCSP: software IRQ, W1C and bus lanes", manualIrqAndWriteOneClearUseBigEndianLanes);
    registry.add("SCSP: Timer C shares IRQ column 7", timerCUsesSharedInterruptLevelColumn);
}
