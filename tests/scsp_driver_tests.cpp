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
#ifdef SATURNBOX_HAS_M68K
    registry.add("SCSP: invalid reset vectors report an error and can recover", invalidResetVectorsFailWithoutAcknowledgingCommands);
    registry.add("SCSP: real 68000 configures slots and emits PCM", realDriverWritesSlotsAndProducesPcm);
#endif
}
