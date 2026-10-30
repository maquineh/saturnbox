#include "test_support.hpp"
#include "saturn/saturn/mmio.hpp"

#include <algorithm>
#include <array>
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

void hostQueueSequenceRequestsAreRecorded() {
    using Sound = saturn::platform::ScspRuntime;
    Sound sound;
    // Host queue pointer, then a sequence-start packet in slot 3.
    REQUIRE(sound.write(Sound::kRamBase + 0x404U, 4, 0x700U));
    const auto packet = 0x700U + 3U * 16U;
    REQUIRE(sound.write(Sound::kRamBase + packet, 1, 0x01U));
    REQUIRE(sound.write(Sound::kRamBase + packet + 2U, 1, 3U));
    REQUIRE(sound.write(Sound::kRamBase + packet + 3U, 1, 2U));
    REQUIRE(sound.write(Sound::kRamBase + packet + 4U, 1, 11U));
    // The batch header packet at 0x700 carries no command.
    REQUIRE(sound.write(Sound::kRamBase + 0x700U, 1, 0x00U));
    // Raising the handshake bit is what makes the runtime look at the queue.
    REQUIRE(sound.write(Sound::kCommandStatus, 1, 0x80U));
    const auto& stats = sound.stats();
    REQUIRE(stats.sequenceRequests == 1U);
    REQUIRE(stats.soundRequests == 1U);
    REQUIRE(stats.lastSoundRequests.size() == 1U);
    const auto& request = stats.lastSoundRequests[0];
    REQUIRE(request.command == 0x01U);
    REQUIRE(request.control == 3U);
    REQUIRE(request.bank == 2U);
    REQUIRE(request.sequence == 11U);
    REQUIRE(request.schedulerTick == 0U);
    REQUIRE(sound.stats().schedulerSteps == 0U);
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

void soundCpuChunkControlsCallGranularity() {
    using Sound = saturn::platform::ScspRuntime;
    const auto run = [](std::uint64_t chunk) {
        Sound sound;
        REQUIRE(sound.enableDriverCpu());
        REQUIRE(sound.write(Sound::kRamBase, 4, 0x8000U));
        REQUIRE(sound.write(Sound::kRamBase + 4U, 4, 0x100U));
        REQUIRE(sound.write(Sound::kRamBase + 0x100U, 2, 0x60FEU));
        sound.setSoundCpuEnabled(true);
        sound.setAudioFrameBlocks(7350); // ~25.6 sound-CPU cycles per step.
        sound.setSoundCpuChunk(chunk);
        for (int step = 0; step < 100; ++step) sound.onBlockTick();
        return sound.stats();
    };
    const auto fine = run(16);
    const auto coarse = run(256);
    // A smaller chunk means more core calls for the same executed budget.
    REQUIRE(fine.soundCpuCalls > coarse.soundCpuCalls);
    REQUIRE(fine.soundCpuCalls >= 90U);
    REQUIRE(coarse.soundCpuCalls <= 12U);
    // Both stay on the 25.6-cycles-per-step budget (2560 cycles in 100 steps).
    REQUIRE(fine.driverCycles >= 2400U);
    REQUIRE(fine.driverCycles < 2900U);
    REQUIRE(coarse.driverCycles >= 2400U);
    REQUIRE(coarse.driverCycles < 2900U);
    Sound defaults;
    REQUIRE(defaults.soundCpuChunk() == Sound::kDefaultSoundCpuChunkCycles);
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

void loadLoopingPcmDriver(saturn::platform::ScspRuntime& sound) {
    using Sound = saturn::platform::ScspRuntime;
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
}

void realtimeDeviceKeepsDriverOnTheGuestBlockClock() {
    using Sound = saturn::platform::ScspRuntime;
    struct Checkpoint {
        Sound::Stats stats;
        std::array<std::uint32_t, 12> slot;
    };
    struct Run {
        std::vector<Checkpoint> checkpoints;
        std::vector<float> pcm;
    };
    const auto execute = [](bool realtime) {
        // This local owns the only active Musashi instance. It is destroyed
        // before execute is called again for the other frontend clock mode.
        Sound sound;
        REQUIRE(sound.enableDriverCpu());
        loadLoopingPcmDriver(sound);
        sound.setAudioFrameBlocks(7350U); // Ten SH-2 steps per audio sample.
        sound.setRealtimeAudioClock(realtime);
        sound.setSoundCpuEnabled(true);
        Run run;
        const auto checkpoint = [&] {
            Checkpoint value{sound.stats(), {}};
            for (std::size_t word = 0U; word < value.slot.size(); ++word)
                value.slot[word] = sound.read(Sound::kRegistersBase +
                    static_cast<std::uint32_t>(word * 2U), 2U).value();
            run.checkpoints.push_back(std::move(value));
        };
        for (unsigned frameIndex = 0U; frameIndex < 2U; ++frameIndex) {
            for (unsigned ticks : {100U, 777U, 2100U}) {
                for (unsigned tick = 0U; tick < ticks; ++tick) sound.onBlockTick();
                REQUIRE(sound.stats().driverCycles > 0U);
                REQUIRE(sound.stats().audioSubFrameSamples > 0U);
                REQUIRE((sound.read(Sound::kRegistersBase, 2U).value() & 0x0800U) != 0U);
                Sound::AudioFrame pending;
                REQUIRE(!sound.consumeAudio(pending));
                checkpoint();
            }
            sound.onVBlank(); // Fill the remainder of the same 735-sample budget.
            checkpoint();
            Sound::AudioFrame frame;
            REQUIRE(sound.consumeAudio(frame));
            REQUIRE(frame.sampleRate == 44100U);
            REQUIRE(frame.channels == 2U);
            REQUIRE(frame.samples.size() == 735U * 2U);
            REQUIRE(sound.stats().audioSamples == (frameIndex + 1U) * 735U);
            REQUIRE(sound.stats().activeVoices == 1U);
            REQUIRE(sound.stats().slotMask == 1U);
            run.pcm.insert(run.pcm.end(), frame.samples.begin(), frame.samples.end());
        }
        return run;
    };
    const auto headless = execute(false);
    const auto device = execute(true);
    REQUIRE(headless.checkpoints.size() == device.checkpoints.size());
    for (std::size_t index = 0U; index < headless.checkpoints.size(); ++index) {
        const auto& a = headless.checkpoints[index];
        const auto& b = device.checkpoints[index];
        REQUIRE(a.stats.driverCycles == b.stats.driverCycles);
        REQUIRE(a.stats.driverPc == b.stats.driverPc);
        REQUIRE(a.stats.driverResets == b.stats.driverResets);
        REQUIRE(a.stats.driverRegisterWrites == b.stats.driverRegisterWrites);
        REQUIRE(a.stats.activeVoices == b.stats.activeVoices);
        REQUIRE(a.stats.slotMask == b.stats.slotMask);
        REQUIRE(a.stats.slotKeyOns == b.stats.slotKeyOns);
        REQUIRE(a.stats.audioBlockTicks == b.stats.audioBlockTicks);
        REQUIRE(a.stats.audioSubFrameSamples == b.stats.audioSubFrameSamples);
        REQUIRE(a.stats.audioFrames == b.stats.audioFrames);
        REQUIRE(a.stats.audioSamples == b.stats.audioSamples);
        REQUIRE(a.slot == b.slot);
    }
    REQUIRE(headless.pcm == device.pcm);
    REQUIRE(std::count_if(device.pcm.begin(), device.pcm.end(), [](float sample) {
        return std::abs(sample) > 0.01F;
    }) > 100);
}

void realDriverWritesSlotsAndProducesPcm() {
    using Sound = saturn::platform::ScspRuntime;
    saturn::platform::MmioInterceptor mmio;
    auto& sound = mmio.scsp();
    REQUIRE(sound.enableDriverCpu());
    loadLoopingPcmDriver(sound);
    REQUIRE(sound.write(Sound::kCommandStatus, 1, 0x80U));
    sound.onSchedulerTick();
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
    registry.add("SCSP: host queue sequence requests are recorded", hostQueueSequenceRequestsAreRecorded);
#ifdef SATURNBOX_HAS_M68K
    registry.add("SCSP: sound CPU executes inside the audio frame", soundCpuRunsInsideTheFrame);
    registry.add("SCSP: sound CPU advances in sub-sample chunks", soundCpuAdvancesInSubSampleChunks);
    registry.add("SCSP: sound CPU chunk controls call granularity", soundCpuChunkControlsCallGranularity);
    registry.add("SCSP: invalid reset vectors report an error and can recover", invalidResetVectorsFailWithoutAcknowledgingCommands);
    registry.add("SCSP: real 68000 configures slots and emits PCM", realDriverWritesSlotsAndProducesPcm);
    registry.add("SCSP: realtime device preserves the real driver block clock",
                 realtimeDeviceKeepsDriverOnTheGuestBlockClock);
#endif
}
