#include "test_support.hpp"
#include "saturn/saturn/mmio.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
void smpcControlsTheSoundCpu() {
    saturn::platform::MmioInterceptor mmio;
#ifndef SATURNBOX_HAS_M68K
    REQUIRE(!mmio.scsp().driverCpuAvailable());
    REQUIRE(!mmio.scsp().enableDriverCpu());
#endif
    REQUIRE(!mmio.smpc().soundCpuEnabled());
    REQUIRE(mmio.write(saturn::platform::SmpcRuntime::kComreg, 1, 6U));
    REQUIRE(mmio.smpc().soundCpuEnabled());
    REQUIRE(mmio.read(saturn::platform::SmpcRuntime::kOreg31, 1) == 6U);
    REQUIRE(mmio.write(saturn::platform::SmpcRuntime::kComreg, 1, 7U));
    REQUIRE(!mmio.smpc().soundCpuEnabled());
}

void audioClockFollowsSchedulerBlocks() {
    using Sound = saturn::platform::ScspRuntime;
    Sound sound;
    // One scheduler step per sample: 735 steps fill a 60 Hz frame.
    sound.setAudioFrameBlocks(735);
    REQUIRE(sound.write(Sound::kRegistersBase + 0x04U, 2, 0U));    // Voice 0 volume.
    REQUIRE(sound.write(Sound::kRegistersBase + 0x00U, 2, 0x0700U)); // Voice 0 pitch.
    for (int step = 0; step < 100; ++step) sound.onBlockTick();
    Sound::AudioFrame frame;
    // Nothing is published until the VBlank that closes the frame.
    REQUIRE(!sound.consumeAudio(frame));
    REQUIRE(sound.write(Sound::kKeyOn, 2, 0x0001U)); // Key on mid-frame.
    for (int step = 0; step < 100; ++step) sound.onBlockTick();
    sound.onVBlank();
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples.size() == 735U * 2U);
    // Audio written before the key-on is silence: the clock advanced inside
    // the frame instead of rendering the whole frame in one burst.
    for (std::size_t sample = 0; sample < 100; ++sample) {
        REQUIRE(frame.samples[sample * 2U] == 0.0F);
        REQUIRE(frame.samples[sample * 2U + 1U] == 0.0F);
    }
    auto audible = false;
    for (std::size_t sample = 200; sample < 735; ++sample) {
        if (std::abs(frame.samples[sample * 2U]) > 0.001F) audible = true;
    }
    REQUIRE(audible);
    REQUIRE(sound.stats().audioBlockTicks == 200U);
    REQUIRE(sound.stats().audioSubFrameSamples == 200U);
    REQUIRE(sound.stats().audioFrames == 1U);
    REQUIRE(sound.stats().audioSamples == 735U);
}

#ifdef SATURNBOX_HAS_M68K
void soundCpuRunsInsideTheFrame() {
    using Sound = saturn::platform::ScspRuntime;
    Sound sound;
    REQUIRE(sound.enableDriverCpu());
    REQUIRE(sound.write(Sound::kRamBase, 4, 0x8000U));
    REQUIRE(sound.write(Sound::kRamBase + 4U, 4, 0x100U));
    // NOP; STOP #0x2700 keeps the sound CPU idle while the clock runs.
    REQUIRE(sound.write(Sound::kRamBase + 0x100U, 4, 0x4E712700U));
    sound.setSoundCpuEnabled(true);
    sound.setAudioFrameBlocks(735);
    for (int step = 0; step < 100; ++step) sound.onBlockTick();
    REQUIRE(sound.stats().driverResets == 1U);
    REQUIRE(sound.stats().driverCycles >= 100U * 256U);
    sound.onVBlank();
    REQUIRE(sound.stats().audioFrames == 1U);
}
#endif

#ifdef SATURNBOX_HAS_M68K
void soundCpuAdvancesInSubSampleChunks() {
    using Sound = saturn::platform::ScspRuntime;
    Sound sound;
    REQUIRE(sound.enableDriverCpu());
    REQUIRE(sound.write(Sound::kRamBase, 4, 0x8000U));
    REQUIRE(sound.write(Sound::kRamBase + 4U, 4, 0x100U));
    // BRA.S to itself: the sound CPU keeps running without touching hardware.
    REQUIRE(sound.write(Sound::kRamBase + 0x100U, 2, 0x60FEU));
    sound.setSoundCpuEnabled(true);
    // 7350 scheduler steps per frame: roughly 25.6 sound-CPU cycles per step.
    sound.setAudioFrameBlocks(7350);
    for (int step = 0; step < 100; ++step) sound.onBlockTick();
    const auto early = sound.stats().driverCycles;
    REQUIRE(early > 0U);
    // Previously the whole 256-cycle sample ran at once; now the chunks are
    // an order of magnitude smaller than a sample's worth of cycles.
    REQUIRE(early < 100U * 256U);
    REQUIRE(early >= 2000U);
    sound.onVBlank();
    // The frame still executes exactly 256 cycles per rendered sample.
    REQUIRE(sound.stats().driverCycles >= 735U * 256U);
    REQUIRE(sound.stats().driverCycles < 735U * 256U + 256U);
    REQUIRE(sound.stats().audioFrames == 1U);
}
#endif

#ifdef SATURNBOX_HAS_M68K
void invalidResetVectorsFailWithoutAcknowledgingCommands() {
    using Sound = saturn::platform::ScspRuntime;
    Sound sound;
    REQUIRE(sound.enableDriverCpu());
    REQUIRE(sound.write(Sound::kRamBase, 4, Sound::kRamSize)); // Legal top-of-RAM stack.
    REQUIRE(sound.write(Sound::kRamBase + 4U, 4, 0x101U));
    sound.setSoundCpuEnabled(true);
    bool rejected{};
    try { sound.onVBlank(); }
    catch (const Sound::DriverError&) { rejected = true; }
    REQUIRE(rejected);
    REQUIRE(sound.stats().driverResets == 0U);
    sound.setSoundCpuEnabled(false);
    REQUIRE(sound.write(Sound::kRamBase + 4U, 4, 0x100U));
    REQUIRE(sound.write(Sound::kRamBase + 0x100U, 4, 0x4E722700U));
    sound.setSoundCpuEnabled(true);
    sound.onVBlank();
    REQUIRE(sound.stats().driverResets == 1U);
}

void realDriverWritesSlotsAndProducesPcm() {
    using Sound = saturn::platform::ScspRuntime;
    saturn::platform::MmioInterceptor mmio;
    auto& sound = mmio.scsp();
    REQUIRE(sound.enableDriverCpu());
    const auto writeRam = [&](std::uint32_t offset, unsigned width, std::uint32_t value) {
        REQUIRE(sound.write(Sound::kRamBase + offset, static_cast<std::uint8_t>(width), value));
    };
    writeRam(0, 4, 0x8000U);
    writeRam(4, 4, 0x100U);
    std::vector<std::uint16_t> code;
    const auto moveWord = [&](std::uint16_t value, std::uint32_t address) {
        code.insert(code.end(), {0x33FCU, value, static_cast<std::uint16_t>(address >> 16U),
                                 static_cast<std::uint16_t>(address)});
    };
    moveWord(0x020FU, 0x100400U); // MVOL full, 512K memory mode.
    moveWord(0x2000U, 0x100002U); // Wave start.
    moveWord(0U, 0x100004U);      // Loop start.
    moveWord(4U, 0x100006U);      // Loop end.
    moveWord(0x001FU, 0x100008U); // Fast attack.
    moveWord(0x0100U, 0x10000CU); // SDIR, no total-level attenuation.
    moveWord(0U, 0x100010U);      // OCT=0, FNS=0.
    moveWord(0xE000U, 0x100016U); // Direct output full, centered.
    moveWord(0x1830U, 0x100000U); // KYONEX, KYONB, forward loop, signed PCM8.
    code.insert(code.end(), {0x13FCU, 0U, 0U, 0x04E0U}); // Guest clears handshake.
    code.insert(code.end(), {0x4E72U, 0x2700U}); // STOP, keep audio hardware running.
    for (std::size_t index = 0; index < code.size(); ++index)
        writeRam(static_cast<std::uint32_t>(0x100U + index * 2U), 2, code[index]);
    for (unsigned index = 0; index < 8U; ++index)
        writeRam(0x2000U + index, 1, (index & 1U) != 0U ? 0x40U : 0xC0U);
    REQUIRE(sound.write(Sound::kCommandStatus, 1, 0x80U));
    saturn::sir::MachineState state;
    sound.onSchedulerTick(state);
    REQUIRE(sound.read(Sound::kCommandStatus, 1) == 0x80U); // HLE cannot acknowledge.
    REQUIRE(mmio.write(saturn::platform::SmpcRuntime::kComreg, 1, 6U));
    sound.onVBlank();
    Sound::AudioFrame frame;
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(sound.read(Sound::kCommandStatus, 1) == 0U);
    REQUIRE(sound.stats().driverResets == 1U);
    REQUIRE(sound.stats().driverCycles >= 735U * 256U);
    REQUIRE(sound.stats().driverRegisterWrites >= 9U);
    REQUIRE(sound.stats().activeVoices == 1U);
    REQUIRE(std::all_of(frame.samples.begin(), frame.samples.end(), [](float value) {
        return std::isfinite(value) && value >= -1.0F && value <= 1.0F;
    }));
    REQUIRE(std::count_if(frame.samples.begin(), frame.samples.end(), [](float value) {
        return std::abs(value) > 0.01F;
    }) > 100);
    const auto beforeStop = sound.stats().driverCycles;
    REQUIRE(mmio.write(saturn::platform::SmpcRuntime::kComreg, 1, 7U));
    sound.onVBlank();
    REQUIRE(sound.stats().driverCycles == beforeStop);
    REQUIRE(mmio.write(saturn::platform::SmpcRuntime::kComreg, 1, 6U));
    sound.onVBlank();
    REQUIRE(sound.stats().driverResets == 2U);
}
#endif
} // namespace

void registerScspDriverTests(test::Registry& registry) {
    registry.add("SCSP: SMPC sound CPU on and off", smpcControlsTheSoundCpu);
    registry.add("SCSP: audio clock advances with scheduler blocks", audioClockFollowsSchedulerBlocks);
#ifdef SATURNBOX_HAS_M68K
    registry.add("SCSP: sound CPU executes inside the audio frame", soundCpuRunsInsideTheFrame);
    registry.add("SCSP: sound CPU advances in sub-sample chunks", soundCpuAdvancesInSubSampleChunks);
    registry.add("SCSP: invalid reset vectors report an error and can recover", invalidResetVectorsFailWithoutAcknowledgingCommands);
    registry.add("SCSP: real 68000 configures slots and emits PCM", realDriverWritesSlotsAndProducesPcm);
#endif
}
