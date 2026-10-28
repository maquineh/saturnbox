#include "test_support.hpp"
#include "saturn/saturn/scsp_synth.hpp"

#include <array>
#include <cmath>

namespace {
using saturn::platform::ScspSynth;
void put(ScspSynth& synth, std::uint32_t address, std::uint16_t value) {
    REQUIRE(synth.write(address, 2, value));
}
void near(float actual, float expected) { REQUIRE(std::abs(actual - expected) < 0.0001F); }
void configure(ScspSynth& synth, unsigned slot, std::uint16_t control,
               std::uint16_t start, std::uint16_t end) {
    const auto base = slot * 0x20;
    put(synth, 0x400, 15);
    put(synth, base, control);
    put(synth, base + 4, start);
    put(synth, base + 6, end);
    put(synth, base + 8, 31); // Instant attack.
    put(synth, base + 10, 0x3c1f); // KRS disabled, fast release.
    put(synth, base + 0x0c, 0x100); // SDIR isolates PCM/loop behavior.
    put(synth, base + 0x16, 0xe000);
}
void trigger(ScspSynth& synth, unsigned slot, std::uint16_t control) {
    put(synth, slot * 0x20, static_cast<std::uint16_t>(control | 0x1800));
}
void pcmByteOrderAndKeyExecute() {
    ScspSynth synth;
    const std::array<std::uint8_t, 8> ram{0x40,0,0xc0,0,0x7f,0xff,0x80,0};
    configure(synth, 0, 0, 0, 4);
    put(synth, 0, 0x800); // KYONB is only preparation.
    near(synth.renderSample(ram)[0], 0);
    REQUIRE(synth.activeVoiceCount() == 0);
    put(synth, 0x20, 0x1000); // KYONEX from another slot executes all slots.
    REQUIRE(synth.activeVoiceCount() == 1);
    REQUIRE(synth.read(0x20, 2) == 0U);
    near(synth.renderSample(ram)[0], 0.5F);
    near(synth.renderSample(ram)[0], -0.5F);
    near(synth.renderSample(ram)[0], 32767.0F / 32768);
    near(synth.renderSample(ram)[0], -1);
    REQUIRE(synth.activeVoiceCount() == 0);
    REQUIRE(synth.read(0, 2) == 0U);
    configure(synth, 0, 0x10, 0, 4);
    trigger(synth, 0, 0x10);
    near(synth.renderSample(ram)[0], 0.5F);
    near(synth.renderSample(ram)[0], 0);
    near(synth.renderSample(ram)[0], -0.5F);
}
void allLoopModesAndPitch() {
    const std::array<std::uint8_t, 5> ram{0,16,32,48,32}; // Matching loop endpoints.
    const std::array<std::array<int, 9>, 4> expected{{
        {{0,16,32,48,0,0,0,0,0}}, {{0,16,32,48,32,48,32,48,32}},
        {{0,16,32,48,32,48,32,48,32}}, {{0,16,32,48,32,48,32,48,32}}}};
    for (unsigned mode = 0; mode < 4; ++mode) {
        ScspSynth synth;
        const auto control = static_cast<std::uint16_t>(0x10 | (mode << 5));
        configure(synth, 0, control, 2, 4);
        trigger(synth, 0, control);
        for (const auto sample : expected[mode]) near(synth.renderSample(ram)[0], sample / 128.0F);
    }
    ScspSynth synth;
    configure(synth, 0, 0x30, 0, 4);
    put(synth, 0x10, 0x7800); // OCT=-1: half sample per output.
    trigger(synth, 0, 0x30);
    near(synth.renderSample(ram)[0], 0);
    near(synth.renderSample(ram)[0], 8.0F / 128);
    near(synth.renderSample(ram)[0], 16.0F / 128);
    put(synth, 0x10, 0x0a00); // OCT=+1, FNS=512: three samples/output.
    near(synth.renderSample(ram)[0], 24.0F / 128);
    near(synth.renderSample(ram)[0], 8.0F / 128);
}
void loopDirectionsAndOvershoot() {
    const std::array<std::uint8_t, 8> ram{0,10,20,30,40,50,60,20};
    const std::array<std::array<int, 10>, 3> expected{{
        {{0,10,20,30,40,50,60,20,30,40}},
        {{0,10,20,60,50,40,30,20,60,50}},
        {{0,10,20,30,40,50,60,20,60,50}}}};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        ScspSynth synth;
        const auto control = static_cast<std::uint16_t>(0x10 | (mode << 5));
        configure(synth, 0, control, 2, 7);
        trigger(synth, 0, control);
        for (const auto sample : expected[mode - 1]) near(synth.renderSample(ram)[0], sample / 128.0F);
        put(synth, 0x10, 0x3bff); // Very large step must remain inside loop safely.
        for (int i = 0; i < 20; ++i) REQUIRE(std::isfinite(synth.renderSample(ram)[0]));
        REQUIRE(synth.stats().outOfRangeSamples == 0);
    }
}
void panVolumeEnvelopeAndRelease() {
    ScspSynth synth;
    const std::array<std::uint8_t, 4> ram{64,64,64,64};
    configure(synth, 0, 0x30, 0, 4);
    put(synth, 0x0c, 0); // Enable EG/TL.
    put(synth, 0x16, 0xef00); // Hard right.
    trigger(synth, 0, 0x30);
    auto out = synth.renderSample(ram);
    near(out[0], 0); near(out[1], 0.5F);
    put(synth, 0x0c, 0x20); // TL=12dB.
    near(synth.renderSample(ram)[1], 0.5F * std::pow(10.0F, -12.0F / 20));
    put(synth, 0x400, 0);
    near(synth.renderSample(ram)[1], 0);
    put(synth, 0x400, 15);
    put(synth, 0, 0x1030); // Clear KYONB and execute release.
    for (unsigned i = 0; i < 200; ++i) (void)synth.renderSample(ram);
    REQUIRE(synth.activeVoiceCount() == 0);
    REQUIRE(synth.stats().keyOffs == 1);
    REQUIRE((synth.read(0x408, 2).value() & 0x7f) == 0x7f);
}
void monitorBusLanesAndAllSlots() {
    ScspSynth synth;
    std::vector<std::uint8_t> ram(0x10000, 1);
    for (unsigned slot = 0; slot < 32; ++slot) configure(synth, slot, 0x830, 0, 0x8000);
    REQUIRE(synth.write(0x3e0, 1, 0x18)); // High byte executes all 32 prepared keys.
    REQUIRE(synth.activeVoiceCount() == 32);
    put(synth, 0x408, 31U << 11);
    for (int i = 0; i < 4096; ++i) (void)synth.renderSample(ram);
    REQUIRE((synth.read(0x408, 2).value() & 0xf800) == 0); // MSLC write-only.
    REQUIRE((synth.read(0x408, 2).value() & 0x780) == 0x80);
    REQUIRE(synth.read(0x408, 1) == 0U);
    REQUIRE(synth.write(0x500, 4, 0x12345678));
    REQUIRE(synth.read(0x500, 4) == 0x12345678U);
    REQUIRE(!synth.write(0xfff, 2, 1));
    REQUIRE(!synth.read(0, 3));
    synth.reset();
    REQUIRE(synth.activeVoiceCount() == 0);
    REQUIRE(synth.stats().renderedSamples == 0);
}
void unsupportedSourcesAreObservableAndInvalidLoopStops() {
    ScspSynth synth;
    const std::array<std::uint8_t, 4> ram{64,64,64,64};
    configure(synth, 0, 0xb0, 0, 4); // Noise source deliberately unsupported.
    put(synth, 0x12, 0x21);
    put(synth, 0x0e, 0x1000);
    put(synth, 0x14, 7);
    trigger(synth, 0, 0xb0);
    near(synth.renderSample(ram)[0], 0);
    REQUIRE(synth.stats().unsupportedSourceSamples == 1);
    REQUIRE(synth.stats().unsupportedLfoSamples == 1);
    REQUIRE(synth.stats().unsupportedFmSamples == 1);
    REQUIRE(synth.stats().unsupportedDspSamples == 1);
    put(synth, 4, 4);
    (void)synth.renderSample(ram);
    REQUIRE(synth.activeVoiceCount() == 0);
    REQUIRE(synth.stats().invalidLoops == 1);
}
}
void registerScspSynthTests(test::Registry& registry) {
    registry.add("SCSP synth: signed PCM and global key execute", pcmByteOrderAndKeyExecute);
    registry.add("SCSP synth: loop modes and fractional signed-octave pitch", allLoopModesAndPitch);
    registry.add("SCSP synth: reverse/pingpong direction and large overshoot", loopDirectionsAndOvershoot);
    registry.add("SCSP synth: direct pan, TL, master and envelope release", panVolumeEnvelopeAndRelease);
    registry.add("SCSP synth: 32 slots, bus lanes and live address monitor", monitorBusLanesAndAllSlots);
    registry.add("SCSP synth: unsupported diagnostics and invalid loops", unsupportedSourcesAreObservableAndInvalidLoopStops);
}
