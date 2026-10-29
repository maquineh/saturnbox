#include "test_support.hpp"
#include "saturn/saturn/scsp_synth.hpp"
#include "saturn/saturn/scsp_timing.hpp"

#include <array>
#include <cstdint>

namespace {
using saturn::platform::ScspSynth;
using saturn::platform::ScspTiming;

void put(ScspSynth& synth, std::uint32_t address, std::uint16_t value) {
    REQUIRE(synth.write(address, 2, value));
}

void dmaCopiesRegistersToSoundRam() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x100> ram{};
    (void)synth.renderSample(ram); // Publish the RAM image.
    // 0x2320 has neither KEYONEX nor the PCM8B bit, so the register keeps it.
    put(synth, 0x0000, 0x2320);
    put(synth, 0x0002, 0x5678);
    // DMEA=0x20, DRGA=0x0000, DTLG=4 bytes, DDIR=1, DEXE=1.
    put(synth, 0x412, 0x0020);
    put(synth, 0x414, 0x0000);
    put(synth, 0x416, static_cast<std::uint16_t>(0x0004U | 0x2000U | 0x1000U));
    REQUIRE(synth.stats().dmaTransfers == 1U);
    REQUIRE(synth.stats().dmaWords == 2U);
    REQUIRE(synth.stats().dmaDropped == 0U);
    REQUIRE(ram[0x20] == 0x23U);
    REQUIRE(ram[0x21] == 0x20U);
    REQUIRE(ram[0x22] == 0x56U);
    REQUIRE(ram[0x23] == 0x78U);
    // DEXE is cleared when the transfer completes.
    REQUIRE((synth.read(0x416, 2).value() & 0x1000U) == 0U);
}

void dmaCopiesSoundRamToRegisters() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x100> ram{};
    ram[0x40] = 0xABU;
    ram[0x41] = 0xCDU;
    (void)synth.renderSample(ram); // Publish the RAM image before the transfer.
    put(synth, 0x412, 0x0040);
    put(synth, 0x414, 0x0100); // DRGA=0x100
    put(synth, 0x416, static_cast<std::uint16_t>(0x0002U | 0x1000U));
    REQUIRE(synth.stats().dmaTransfers == 1U);
    REQUIRE(synth.read(0x100, 2) == 0xABCDU);
    REQUIRE((synth.read(0x416, 2).value() & 0x1000U) == 0U);
}

void dmaGateClearsTheRegisterDestination() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x100> ram{};
    (void)synth.renderSample(ram);
    put(synth, 0x100, 0x1234);
    put(synth, 0x412, 0x0000);
    put(synth, 0x414, 0x0100);
    // DGATE=1 with DDIR=0 clears DRGA instead of copying.
    put(synth, 0x416, static_cast<std::uint16_t>(0x0004U | 0x4000U | 0x1000U));
    REQUIRE(synth.stats().dmaTransfers == 1U);
    REQUIRE(synth.read(0x100, 2) == 0U);
}

void dmaRequestedBeforeAnySampleRunsOnTheNextSample() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x100> ram{};
    put(synth, 0x0000, 0x2320);
    put(synth, 0x412, 0x0000);
    put(synth, 0x414, 0x0000);
    put(synth, 0x416, static_cast<std::uint16_t>(0x0002U | 0x2000U | 0x1000U));
    // No RAM image yet: the transfer is deferred, not dropped.
    REQUIRE(synth.dmaPending());
    REQUIRE(synth.stats().dmaTransfers == 0U);
    (void)synth.renderSample(ram);
    REQUIRE(!synth.dmaPending());
    REQUIRE(synth.stats().dmaTransfers == 1U);
    REQUIRE(ram[0] == 0x23U);
    REQUIRE(ram[1] == 0x20U);
}

void dmaTransferLargerThanTheImageCountsDroppedWords() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x10> ram{};
    (void)synth.renderSample(ram);
    put(synth, 0x412, 0x000EU);
    put(synth, 0x414, 0x0000);
    put(synth, 0x416, static_cast<std::uint16_t>(0x0008U | 0x2000U | 0x1000U));
    REQUIRE(synth.stats().dmaTransfers == 1U);
    REQUIRE(synth.stats().dmaWords == 1U);   // 0x0E..0x0F fits.
    REQUIRE(synth.stats().dmaDropped == 3U); // 0x10, 0x12 and 0x14 do not.
}

void dmaRunsImmediatelyOnceTheImageIsPublished() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x100> ram{};
    // Publishing the image up front removes the "deferred to the first sample"
    // case: a transfer during sound-driver boot runs at once.
    synth.setSoundRam(ram);
    put(synth, 0x0000, 0x2320);
    put(synth, 0x412, 0x0010);
    put(synth, 0x414, 0x0000);
    put(synth, 0x416, static_cast<std::uint16_t>(0x0002U | 0x2000U | 0x1000U));
    REQUIRE(!synth.dmaPending());
    REQUIRE(synth.stats().dmaTransfers == 1U);
    REQUIRE(ram[0x10] == 0x23U);
    REQUIRE(ram[0x11] == 0x20U);
}

void midiOutputKeepsTheTransmitRate() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kMidiOutput, 1, 0xF0U));
    REQUIRE(timing.write(ScspTiming::kMidiOutput, 1, 0x7EU));
    REQUIRE((timing.read(ScspTiming::kMidiStatus, 2).value() & 0x0800U) == 0U);
    // 31250 baud is ~14.1 audio samples per byte, so one sample sends nothing.
    timing.advanceSamples(1);
    REQUIRE((timing.read(ScspTiming::kMidiStatus, 2).value() & 0x0800U) == 0U);
    timing.advanceSamples(14);
    REQUIRE((timing.read(ScspTiming::kMidiStatus, 2).value() & 0x0800U) == 0U);
    timing.advanceSamples(14);
    REQUIRE((timing.read(ScspTiming::kMidiStatus, 2).value() & 0x0800U) != 0U);
    REQUIRE(timing.takeMidiOutput() == 0U);
}

void midiOutputDrainsOneBytePerSerialFrame() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kMidiOutput, 1, 0x90U));
    unsigned samples{};
    while ((timing.read(ScspTiming::kMidiStatus, 2).value() & 0x0800U) == 0U &&
           samples < 100U) {
        timing.advanceSamples(1);
        ++samples;
    }
    REQUIRE(samples >= 14U);
    REQUIRE(samples <= 16U);
}

void midiInputRaisesTheInterruptFactorAndPopsOnRead() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kScilv0 + 1U, 1, 1U << 3U)); // MIDI on level 1.
    REQUIRE(timing.write(ScspTiming::kScieb, 2, 1U << 3U));
    REQUIRE(timing.irqLevel() == 0U);
    timing.pushMidiInput(0x90U);
    REQUIRE(timing.midiInputPending() == 1U);
    REQUIRE((timing.read(ScspTiming::kScipd, 2).value() & 0x0008U) != 0U);
    REQUIRE(timing.irqLevel() == 1U);
    const auto status = timing.read(ScspTiming::kMidiStatus, 2).value();
    REQUIRE((status & 0x0100U) == 0U);          // MIEMPTY clear.
    REQUIRE((status & 0x00FFU) == 0x90U);       // MIBUF returns the byte.
    // Reading MIBUF consumed it, which clears the pending factor.
    REQUIRE(timing.midiInputPending() == 0U);
    REQUIRE((timing.read(ScspTiming::kScipd, 2).value() & 0x0008U) == 0U);
    REQUIRE(timing.irqLevel() == 0U);
}

void midiInputOverflowIsReported() {
    ScspTiming timing;
    for (unsigned index = 0; index < ScspTiming::kMidiFifoSize; ++index)
        timing.pushMidiInput(static_cast<std::uint8_t>(index));
    timing.pushMidiInput(0x55U);
    REQUIRE(timing.stats().midiOverflows == 1U);
    REQUIRE((timing.read(ScspTiming::kMidiStatus, 2).value() & 0x0400U) != 0U);
}

void dmaCompletionRaisesTheInterruptFactor() {
    ScspTiming timing;
    REQUIRE(timing.write(ScspTiming::kScieb, 2, 1U << 4U));
    timing.notifyDmaCompletion();
    REQUIRE(timing.stats().dmaCompletions == 1U);
    REQUIRE((timing.read(ScspTiming::kScipd, 2).value() & 0x0010U) != 0U);
    // SCIRE clears the completion factor like any other pending source.
    REQUIRE(timing.write(ScspTiming::kScire, 2, 1U << 4U));
    REQUIRE((timing.read(ScspTiming::kScipd, 2).value() & 0x0010U) == 0U);
}

void dmaCompletionHookFiresAfterATransfer() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x40> ram{};
    unsigned completions{};
    synth.setDmaCompletionHook([&completions] { ++completions; });
    put(synth, 0x412, 0x0000);
    put(synth, 0x414, 0x0000);
    put(synth, 0x416, static_cast<std::uint16_t>(0x0002U | 0x2000U | 0x1000U));
    REQUIRE(synth.dmaPending()); // No RAM image yet, so no hook call.
    REQUIRE(completions == 0U);
    (void)synth.renderSample(ram);
    REQUIRE(completions == 1U);
}
} // namespace

void registerScspDmaTests(test::Registry& registry) {
    registry.add("SCSP DMA: register to sound RAM", dmaCopiesRegistersToSoundRam);
    registry.add("SCSP DMA: sound RAM to register", dmaCopiesSoundRamToRegisters);
    registry.add("SCSP DMA: gate mode clears the destination", dmaGateClearsTheRegisterDestination);
    registry.add("SCSP DMA: deferred until the RAM image is known", dmaRequestedBeforeAnySampleRunsOnTheNextSample);
    registry.add("SCSP DMA: immediate once the image is published", dmaRunsImmediatelyOnceTheImageIsPublished);
    registry.add("SCSP DMA: short image counts dropped words", dmaTransferLargerThanTheImageCountsDroppedWords);
    registry.add("SCSP MIDI: output status and transmit rate", midiOutputKeepsTheTransmitRate);
    registry.add("SCSP MIDI: one byte per serial frame", midiOutputDrainsOneBytePerSerialFrame);
    registry.add("SCSP MIDI: input raises IRQ and pops on read", midiInputRaisesTheInterruptFactorAndPopsOnRead);
    registry.add("SCSP MIDI: input overflow is reported", midiInputOverflowIsReported);
    registry.add("SCSP DMA: completion raises the interrupt factor", dmaCompletionRaisesTheInterruptFactor);
    registry.add("SCSP DMA: completion hook fires after a transfer", dmaCompletionHookFiresAfterATransfer);
}
