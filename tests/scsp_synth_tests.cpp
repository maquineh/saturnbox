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
    std::array<std::uint8_t, 8> ram{0x40,0,0xc0,0,0x7f,0xff,0x80,0};
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
    std::array<std::uint8_t, 5> ram{0,16,32,48,32}; // Matching loop endpoints.
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
    std::array<std::uint8_t, 8> ram{0,10,20,30,40,50,60,20};
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
    std::array<std::uint8_t, 4> ram{64,64,64,64};
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
void noiseSourceIsSynthesized() {
    ScspSynth synth;
    std::array<std::uint8_t, 4> ram{64,64,64,64};
    configure(synth, 0, 0xb0, 0, 4); // SSCTL=1 selects the internal noise source.
    trigger(synth, 0, 0xb0);
    float peak = 0;
    int audible = 0;
    for (int i = 0; i < 128; ++i) {
        const auto sample = synth.renderSample(ram)[0];
        REQUIRE(std::isfinite(sample));
        peak = std::max(peak, std::abs(sample));
        if (std::abs(sample) > 0.01F) ++audible;
    }
    REQUIRE(synth.stats().noiseSamples == 128);
    REQUIRE(synth.stats().unsupportedSourceSamples == 0);
    REQUIRE(audible > 100);
    REQUIRE(peak > 0.3F);
    REQUIRE(synth.activeVoiceCount() == 1);
    REQUIRE(synth.stats().activeSlotMask == 0x1U);
    // SSCTL=3 stays silent: the Sega manual marks it unusable.
    ScspSynth invalid;
    configure(invalid, 0, 0x1b0, 0, 4); // SSCTL=3 (bits 7 and 8).
    trigger(invalid, 0, 0x1b0);
    near(invalid.renderSample(ram)[0], 0);
    REQUIRE(invalid.stats().unsupportedSourceSamples == 1);
}

void pitchAndAmplitudeLfoModulateTheSlot() {
    std::array<std::uint8_t, 64> ramp{};
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<std::uint8_t>(index * 4U);
    const auto prepare = [](ScspSynth& synth) {
        configure(synth, 0, 0x30, 0, 63);
        trigger(synth, 0, 0x30);
    };
    ScspSynth plain;
    prepare(plain);
    ScspSynth pitchModulated;
    prepare(pitchModulated);
    // LFOF=23 (10.8 Hz), saw pitch waveform, 494 cents of depth.
    put(pitchModulated, 0x12, static_cast<std::uint16_t>((23U << 10) | (7U << 5)));
    bool pitchDiffers = false;
    for (int i = 0; i < 256; ++i) {
        const auto reference = plain.renderSample(ramp)[0];
        const auto modulated = pitchModulated.renderSample(ramp)[0];
        REQUIRE(std::isfinite(reference));
        REQUIRE(std::isfinite(modulated));
        if (std::abs(reference - modulated) > 0.01F) pitchDiffers = true;
    }
    REQUIRE(pitchDiffers);
    REQUIRE(pitchModulated.stats().pitchLfoSamples == 256);
    REQUIRE(plain.stats().pitchLfoSamples == 0);

    ScspSynth amplitudeModulated;
    prepare(amplitudeModulated);
    // Same rate, saw amplitude waveform, 24 dB of depth.
    put(amplitudeModulated, 0x12, static_cast<std::uint16_t>((23U << 10) | (1U << 3) | 7U));
    float loudest = 0;
    float quietest = 1;
    for (int i = 0; i < 256; ++i) {
        const auto sample = std::abs(amplitudeModulated.renderSample(ramp)[0]);
        REQUIRE(std::isfinite(sample));
        loudest = std::max(loudest, sample);
        quietest = std::min(quietest, sample);
    }
    REQUIRE(amplitudeModulated.stats().amplitudeLfoSamples == 256);
    REQUIRE(amplitudeModulated.stats().pitchLfoSamples == 0);
    REQUIRE(loudest > quietest * 2.0F);
}

void modulationRingBufferAffectsPhase() {
    std::array<std::uint8_t, 64> ramp{};
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<std::uint8_t>(index * 4U);
    const auto prepare = [](ScspSynth& synth) {
        configure(synth, 0, 0x30, 0, 63);
        trigger(synth, 0, 0x30);
    };
    ScspSynth plain;
    prepare(plain);
    ScspSynth modulated;
    prepare(modulated);
    // MDL=15, MDXSL=MDYSL=63: full-depth phase modulation from the ring buffer.
    put(modulated, 0x0e, 0xF03F);
    bool differs = false;
    for (int i = 0; i < 256; ++i) {
        const auto reference = plain.renderSample(ramp)[0];
        const auto changed = modulated.renderSample(ramp)[0];
        REQUIRE(std::isfinite(reference));
        REQUIRE(std::isfinite(changed));
        if (std::abs(reference - changed) > 0.01F) differs = true;
    }
    REQUIRE(differs);
    REQUIRE(modulated.stats().fmSamples == 256);
    REQUIRE(plain.stats().fmSamples == 0);
}

void effectDspReturnsSendThroughEffectRegisters() {
    ScspSynth synth;
    std::array<std::uint8_t, 8> ram{64,64,64,64,64,64,64,64};
    // The shifter consumes the accumulator of the *previous* step, so one step
    // produces ACC and the next moves it into the effect registers.
    put(synth, 0x700, 0x7FFF); // COEF[0] >> 3 is the largest positive mantissa.
    put(synth, 0x800, 0x0000);
    put(synth, 0x802, 0xA800); // XSEL=1 (MIXS), YSEL=1 (COEF), IRA=0x20.
    put(synth, 0x804, 0x0030); // SHIFT=3.
    put(synth, 0x806, 0x0000); // COEF index 0, MASA=0.
    put(synth, 0x808, 0x0000);
    put(synth, 0x80A, 0x0000);
    put(synth, 0x80C, 0x1030); // EWT=1, EWA=0, SHIFT=3.
    put(synth, 0x80E, 0x0000);
    put(synth, 0xBF0, 0x0000); // Start the program without adding a step.
    REQUIRE(!synth.dsp().stopped());
    REQUIRE(synth.dsp().lastStep() == 2);
    configure(synth, 0, 0x30, 0, 4);
    put(synth, 0x0c, 0x100);   // SDIR: unity envelope, no TL attenuation.
    put(synth, 0x14, 0x0007);  // ISEL=0, IMXL=7: full send into the DSP.
    put(synth, 0x16, 0x00E0);  // DISDL=0, EFSDL=7, EFPAN=0.
    trigger(synth, 0, 0x30);
    const auto out = synth.renderSample(ram);
    // The direct path is muted, so only the effect return can produce audio.
    REQUIRE(std::abs(out[0] - 0.5F) < 0.002F);
    REQUIRE(std::abs(out[1] - 0.5F) < 0.002F);
    REQUIRE(synth.stats().dspSendSamples == 1);
    REQUIRE(synth.stats().dspEffectSamples == 1);
    REQUIRE(synth.dsp().stats().steps == 2);
    REQUIRE(synth.dsp().stats().samples == 1);
}

void effectDspWritesDelayMemoryIntoSoundRam() {
    ScspSynth synth;
    std::array<std::uint8_t, 0x400> ram{};
    for (std::size_t index = 0; index < 8U; ++index) ram[index] = 64;
    put(synth, 0x700, 0x7FFF);
    put(synth, 0x780, 0x0100); // MADRS[0]: delay-memory word 0x100.
    // Step 0 produces ACC from the slot send; step 1 stores it to delay memory.
    put(synth, 0x800, 0x0000);
    put(synth, 0x802, 0xA800);
    put(synth, 0x804, 0x0030);
    put(synth, 0x806, 0x0000);
    put(synth, 0x808, 0x0000);
    put(synth, 0x80A, 0x0000);
    put(synth, 0x80C, 0x4030); // MWT=1, SHIFT=3.
    put(synth, 0x80E, 0x0000);
    put(synth, 0xBF0, 0x0000);
    REQUIRE(synth.dsp().lastStep() == 2);
    configure(synth, 0, 0x30, 0, 4);
    put(synth, 0x14, 0x0007);
    put(synth, 0x16, 0x00E0);
    trigger(synth, 0, 0x30);
    (void)synth.renderSample(ram);
    REQUIRE(synth.dsp().stats().writes == 1);
    REQUIRE(synth.dsp().stats().droppedWrites == 0);
    // Word 0x100 lives at bytes 0x200-0x201, outside the PCM data at 0x0.
    REQUIRE((ram[0x200] | ram[0x201]) != 0);
}
}
void registerScspSynthTests(test::Registry& registry) {
    registry.add("SCSP synth: signed PCM and global key execute", pcmByteOrderAndKeyExecute);
    registry.add("SCSP synth: loop modes and fractional signed-octave pitch", allLoopModesAndPitch);
    registry.add("SCSP synth: reverse/pingpong direction and large overshoot", loopDirectionsAndOvershoot);
    registry.add("SCSP synth: direct pan, TL, master and envelope release", panVolumeEnvelopeAndRelease);
    registry.add("SCSP synth: 32 slots, bus lanes and live address monitor", monitorBusLanesAndAllSlots);
    registry.add("SCSP synth: internal noise source and invalid SSCTL", noiseSourceIsSynthesized);
    registry.add("SCSP synth: pitch and amplitude LFO modulation", pitchAndAmplitudeLfoModulateTheSlot);
    registry.add("SCSP synth: phase modulation from the slot ring buffer", modulationRingBufferAffectsPhase);
    registry.add("SCSP synth: effect DSP send and effect returns", effectDspReturnsSendThroughEffectRegisters);
    registry.add("SCSP synth: effect DSP delay memory in sound RAM", effectDspWritesDelayMemoryIntoSoundRam);
}
