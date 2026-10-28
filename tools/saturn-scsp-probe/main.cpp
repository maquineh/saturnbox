#include "saturn/saturn/m68k.hpp"
#include "saturn/saturn/scsp_synth.hpp"
#include "saturn/saturn/scsp_timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void writeLe16(std::ostream& output, std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value), static_cast<char>(value >> 8U)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void writeLe32(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8U),
                                    static_cast<char>(value >> 16U), static_cast<char>(value >> 24U)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void writeWavHeader(std::ostream& output, std::uint32_t dataBytes) {
    output.seekp(0);
    output.write("RIFF", 4); writeLe32(output, 36U + dataBytes); output.write("WAVE", 4);
    output.write("fmt ", 4); writeLe32(output, 16U); writeLe16(output, 1U);
    writeLe16(output, 2U); writeLe32(output, 44100U); writeLe32(output, 176400U);
    writeLe16(output, 4U); writeLe16(output, 16U); output.write("data", 4);
    writeLe32(output, dataBytes);
}
void writePcm16(std::ostream& output, const std::array<float, 2>& sample) {
    for (const auto channel : sample) {
        const auto clipped = std::clamp(channel, -1.0F, 1.0F);
        const auto scaled = static_cast<std::int16_t>(std::lrint(clipped * 32767.0F));
        writeLe16(output, static_cast<std::uint16_t>(scaled));
    }
}
}

// Bounded offline driver execution. This bus preserves register writes for
// inspection. Optional WAV output renders basic PCM voices; DSP is not rendered.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: saturn-scsp-probe <driver-or-sound-ram.bin> [cycles=1000000] [register-writes.csv] [--no-timers|--test-master-volume|--replay-pending] [--mark-loaded] [--dump-final-ram PATH] [--dump-registers PATH] [--wav PATH]\n";
        return 1;
    }
    try {
        std::string mode;
        std::optional<std::filesystem::path> finalRamPath, finalRegistersPath, wavPath;
        bool markLoaded{};
        int option = 2;
        std::uint64_t budget = 1000000ULL;
        if (option < argc && argv[option][0] != '-') budget = std::stoull(argv[option++]);
        std::ofstream trace;
        if (option < argc && argv[option][0] != '-') {
            trace.open(argv[option++]);
            if (!trace) throw std::runtime_error("cannot open register trace output");
            trace << "cycles_before_batch,pc,address,width,value\n";
        }
        while (option < argc) {
            const std::string argument = argv[option++];
            if (argument == "--no-timers" || argument == "--test-master-volume" || argument == "--replay-pending") {
                if (!mode.empty()) throw std::runtime_error("only one probe mode may be selected");
                mode = argument;
            } else if (argument == "--mark-loaded") {
                if (markLoaded) throw std::runtime_error("--mark-loaded was specified more than once");
                markLoaded = true;
            } else if (argument == "--dump-final-ram" || argument == "--dump-registers") {
                if (option >= argc) throw std::runtime_error(argument + " requires a path");
                auto& destination = argument == "--dump-final-ram" ? finalRamPath : finalRegistersPath;
                if (destination) throw std::runtime_error(argument + " was specified more than once");
                destination = std::filesystem::path(argv[option++]);
            } else if (argument == "--wav") {
                if (option >= argc) throw std::runtime_error("--wav requires a path");
                if (wavPath) throw std::runtime_error("--wav was specified more than once");
                wavPath = std::filesystem::path(argv[option++]);
            } else {
                throw std::runtime_error("unknown option: " + argument);
            }
        }
        std::ifstream source(argv[1], std::ios::binary);
        if (!source) throw std::runtime_error("cannot open sound RAM input");
        std::vector<std::uint8_t> ram{std::istreambuf_iterator<char>(source), {}};
        if (ram.size() < 8U || ram.size() > 0x80000U)
            throw std::runtime_error("input must contain 8..524288 bytes");
        ram.resize(0x80000U);
        if (budget == 0U || budget > 1000000000ULL)
            throw std::runtime_error("cycle budget must be 1..1000000000");
        if (markLoaded && mode != "--replay-pending")
            throw std::runtime_error("--mark-loaded requires --replay-pending");
        std::vector<std::uint8_t> registers(0x1000U);
        saturn::platform::ScspTiming timing;
        saturn::platform::ScspSynth synth;
        std::ofstream wav;
        if (wavPath) {
            if (std::filesystem::exists(*wavPath))
                throw std::runtime_error("WAV output already exists: " + wavPath->string());
            wav.open(*wavPath, std::ios::binary | std::ios::trunc);
            if (!wav) throw std::runtime_error("cannot open WAV output: " + wavPath->string());
            writeWavHeader(wav, 0U);
        }
        const bool timersEnabled = mode != "--no-timers";
        std::uint64_t registerWrites{}, slotWrites{}, keyOnRequests{}, unmappedReads{}, unmappedWrites{};
        std::size_t loadedEntriesMarked{};
        bool loadedMapSeen{};
        constexpr std::array<std::array<std::uint32_t, 3>, 10> verifiedMapEntries{{
            {{0x00U, 0x0B000U, 0x1DD60U}}, {{0x10U, 0x28D60U, 0x006FCU}},
            {{0x11U, 0x2945CU, 0x001F8U}}, {{0x12U, 0x29654U, 0x003C6U}},
            {{0x13U, 0x29A1AU, 0x00480U}}, {{0x14U, 0x29E9AU, 0x00452U}},
            {{0x01U, 0x2A2ECU, 0x2114AU}}, {{0x02U, 0x4B436U, 0x0F4BCU}},
            {{0x03U, 0x5A8F2U, 0x12B58U}}, {{0x04U, 0x6D44AU, 0x0861CU}}
        }};
        saturn::platform::M68k* running{};
        const auto byteAt = [&](std::uint32_t address) -> std::uint8_t {
            address &= 0xFFFFFFU;
            if (address < ram.size()) return ram[address];
            if (address >= 0x100000U && address < 0x101000U) {
                if (timersEnabled) {
                    const auto timed = timing.read(static_cast<std::uint16_t>(address - 0x100000U), 1U);
                    if (timed) return static_cast<std::uint8_t>(*timed);
                }
                if (const auto value = synth.read(address - 0x100000U, 1U))
                    return static_cast<std::uint8_t>(*value);
                return registers[address - 0x100000U];
            }
            ++unmappedReads;
            return 0xFFU;
        };
        const auto read = [&](std::uint32_t address, unsigned width) {
            std::uint32_t value{};
            for (unsigned byte = 0; byte < width; ++byte)
                value = (value << 8U) | byteAt(address + byte);
            return value;
        };
        const auto markVerifiedLoadedMap = [&]() {
            if (!markLoaded || loadedMapSeen) return;
            const auto map = read(0x408U, 4U);
            if (map != 0x500U) throw std::runtime_error("map CRNT pointer is not the verified 0x500 table");
            for (std::size_t index = 0; index < verifiedMapEntries.size(); ++index) {
                const auto offset = static_cast<std::size_t>(map) + index * 8U;
                const auto& expected = verifiedMapEntries[index];
                const auto start = (static_cast<std::uint32_t>(ram[offset + 1U]) << 16U) |
                                   (static_cast<std::uint32_t>(ram[offset + 2U]) << 8U) |
                                   ram[offset + 3U];
                const auto area = (static_cast<std::uint32_t>(ram[offset + 4U] & 0x7FU) << 24U) |
                                  (static_cast<std::uint32_t>(ram[offset + 5U]) << 16U) |
                                  (static_cast<std::uint32_t>(ram[offset + 6U]) << 8U) |
                                  ram[offset + 7U];
                if (ram[offset] != expected[0] || start != expected[1] || area != expected[2] ||
                    start > ram.size() || area > ram.size() - start)
                    throw std::runtime_error("CRNT map does not match a validated Rockman X4 bank range");
            }
            if (ram[map + verifiedMapEntries.size() * 8U] != 0xFFU)
                throw std::runtime_error("CRNT map terminator is not at the verified offset");
            for (std::size_t index = 0; index < verifiedMapEntries.size(); ++index) {
                const auto flag = static_cast<std::size_t>(map) + index * 8U + 4U;
                if ((ram[flag] & 0x80U) == 0U) {
                    ram[flag] |= 0x80U;
                    ++loadedEntriesMarked;
                }
            }
            loadedMapSeen = true;
        };
        const auto write = [&](std::uint32_t address, unsigned width, std::uint32_t value) {
            address &= 0xFFFFFFU;
            if (address >= 0x100000U && address < 0x101000U) {
                ++registerWrites;
                if (address < 0x100400U) ++slotWrites;
                if (trace.is_open())
                    trace << std::dec << running->stats().cyclesExecuted << ",0x"
                          << std::hex << running->pc() << ",0x" << address << ','
                          << std::dec << width << ",0x" << std::hex << value << '\n';
            }
            for (unsigned byte = 0; byte < width; ++byte) {
                const auto destination = (address + byte) & 0xFFFFFFU;
                const auto data = static_cast<std::uint8_t>(value >> ((width - byte - 1U) * 8U));
                if (destination < ram.size()) ram[destination] = data;
                else if (destination >= 0x100000U && destination < 0x101000U) {
                    registers[destination - 0x100000U] = data;
                    if (timersEnabled)
                        static_cast<void>(timing.write(
                            static_cast<std::uint16_t>(destination - 0x100000U), 1U, data));
                }
                else ++unmappedWrites;
                if (destination == 0x550U && data == 0xFFU) markVerifiedLoadedMap();
            }
            if (address < 0x100400U && address >= 0x100000U && (address & 0x1FU) < 2U) {
                const auto slot = (address - 0x100000U) & ~0x1FU;
                const auto control = (registers[slot] << 8U) | registers[slot + 1U];
                if ((control & 0x1800U) == 0x1800U) ++keyOnRequests;
            }
            if (address >= 0x100000U && address < 0x101000U) {
                const auto offset = address - 0x100000U;
                static_cast<void>(synth.write(offset, static_cast<std::uint8_t>(width), value));
                for (unsigned byte = 0; byte < width && offset + byte < registers.size(); ++byte) {
                    if (const auto current = synth.read(offset + byte, 1U))
                        registers[offset + byte] = static_cast<std::uint8_t>(*current);
                }
            }
            if (timersEnabled && running) running->setIrq(timing.irqLevel());
        };
        const auto resetSp = read(0U, 4U), resetPc = read(4U, 4U);
        std::vector<std::uint8_t> pending;
        if (mode == "--replay-pending") {
            const auto host = read(0x404U, 4U);
            if (host < 0x500U || host > ram.size() - 0x100U)
                throw std::runtime_error("input has no valid command queue pointer");
            pending.assign(ram.begin() + host, ram.begin() + host + 0x80U);
            if (std::none_of(pending.begin(), pending.end(), [](auto v) { return v != 0U; }))
                throw std::runtime_error("input command queue is empty");
        }
        if (resetSp >= ram.size() || resetPc >= ram.size() ||
            ((resetPc | resetSp) & 1U) != 0U)
            throw std::runtime_error("reset vectors are outside sound RAM or misaligned");
        saturn::platform::M68k cpu({
            [&](auto a) { return static_cast<std::uint8_t>(read(a, 1U)); },
            [&](auto a) { return static_cast<std::uint16_t>(read(a, 2U)); },
            [&](auto a) { return read(a, 4U); },
            [&](auto a, std::uint8_t v) { write(a, 1U, v); },
            [&](auto a, std::uint16_t v) { write(a, 2U, v); },
            [&](auto a, std::uint32_t v) { write(a, 4U, v); }});
        running = &cpu;
        cpu.reset();
        std::uint32_t sampleCycleRemainder{};
        bool injected{};
        while (cpu.stats().cyclesExecuted < budget) {
            if ((mode == "--test-master-volume" || mode == "--replay-pending") && !injected &&
                cpu.stats().cyclesExecuted >= 100000U) {
                const auto host = read(0x404U, 4U);
                if (host < 0x500U || host > ram.size() - 0x100U)
                    throw std::runtime_error("driver host command pointer is invalid");
                if (mode == "--test-master-volume") {
                    std::fill_n(ram.begin() + host, 0x80U, 0U);
                    ram[host] = 0x82U;
                    ram[host + 2U] = 5U;
                } else std::copy(pending.begin(), pending.end(), ram.begin() + host);
                ram[0x4E0U] |= 0x80U;
                injected = true;
            }
            const auto remaining = budget - cpu.stats().cyclesExecuted;
            // Saturn's sound CPU is 256 * 44100 Hz. Preserve instruction
            // overshoot instead of treating each execute call as one sample.
            const auto cycles = cpu.execute(static_cast<std::uint32_t>(
                std::min<std::uint64_t>(remaining, 256U - sampleCycleRemainder)));
            if (cycles == 0U)
                throw std::runtime_error("68k core made no cycle progress");
            sampleCycleRemainder += cycles;
            const auto sampleCount = sampleCycleRemainder / 256U;
            sampleCycleRemainder %= 256U;
            if (timersEnabled) {
                timing.advanceSamples(sampleCount);
                cpu.setIrq(timing.irqLevel());
            }
            if (wav.is_open()) {
                for (std::uint64_t sample = 0; sample < sampleCount; ++sample)
                    writePcm16(wav, synth.renderSample(ram));
            }
        }
        if (trace.is_open()) {
            trace.flush();
            if (!trace) throw std::runtime_error("failed to write register trace");
        }
        if (wav.is_open()) {
            const auto sampleCount = synth.stats().renderedSamples;
            const auto dataBytes = static_cast<std::uint32_t>(sampleCount * 4U);
            writeWavHeader(wav, dataBytes);
            wav.flush();
            if (!wav) throw std::runtime_error("failed to finalize WAV output");
        }
        const auto writeDump = [](const std::filesystem::path& path,
                                  const std::vector<std::uint8_t>& data) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open dump output: " + path.string());
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!output) throw std::runtime_error("failed to write dump output: " + path.string());
        };
        if (finalRamPath) writeDump(*finalRamPath, ram);
        if (finalRegistersPath) writeDump(*finalRegistersPath, registers);
        std::cout << "reset_sp=0x" << std::hex << resetSp << " reset_pc=0x" << resetPc
                  << " final_pc=0x" << cpu.pc() << std::dec
                  << " cycles=" << cpu.stats().cyclesExecuted
                  << " register_writes=" << registerWrites << " slot_writes=" << slotWrites
                  << " slot_key_on_requests=" << keyOnRequests
                  << " synthesized_key_ons=" << synth.stats().keyOns
                  << " active_voices=" << synth.activeVoiceCount()
                  << " unmapped_reads=" << unmappedReads << " unmapped_writes=" << unmappedWrites
                  << " timer_b_events=" << timing.stats().timers[1].expirations
                  << "\nDSP synthesis is not implemented in this probe.\n";
        if (wavPath) {
            const auto& stats = synth.stats();
            std::cout << "wav=" << wavPath->string() << " samples=" << stats.renderedSamples
                      << " unsupported_dsp_samples=" << stats.unsupportedDspSamples
                      << " unsupported_lfo_samples=" << stats.unsupportedLfoSamples
                      << " out_of_range_samples=" << stats.outOfRangeSamples << '\n';
        }
        const auto hostAfter = read(0x404U, 4U);
        std::size_t nonEmptyPackets{};
        if (hostAfter < ram.size() - 0x80U) {
            for (std::size_t packet = hostAfter; packet < hostAfter + 0x80U; packet += 16U)
                if (ram[packet] != 0U) ++nonEmptyPackets;
        }
        std::size_t slotsWithKeyOn{};
        for (std::size_t slot = 0; slot < 32U; ++slot) {
            const auto control = (static_cast<std::uint16_t>(registers[slot * 0x20U]) << 8U) |
                                 registers[slot * 0x20U + 1U];
            if ((control & 0x1800U) == 0x1800U) ++slotsWithKeyOn;
        }
        std::cout << "interface_host=0x" << std::hex << hostAfter
                  << " interface_status_4e0=0x" << static_cast<unsigned>(ram[0x4E0U])
                  << " interface_indicator_4e1=0x" << static_cast<unsigned>(ram[0x4E1U])
                  << std::dec << " nonempty_packets=" << nonEmptyPackets
                  << " final_slots_with_KYONB_and_KYONEX=" << slotsWithKeyOn
                  << " driver_error_bitmap_420=0x" << std::hex << read(0x420U, 4U) << std::dec
                  << " map_entries_marked_loaded=" << loadedEntriesMarked
                  << " verified_map_seen=" << (loadedMapSeen ? 1 : 0) << '\n';
        if (mode == "--test-master-volume") {
            const auto volume = registers[0x401U] & 0xFU;
            const bool passed = injected && volume == 5U && (ram[0x4E0U] & 0x80U) == 0U;
            std::cout << "master_volume_test=" << (passed ? "pass" : "fail")
                      << " volume=" << static_cast<unsigned>(volume)
                      << " command_status=" << static_cast<unsigned>(ram[0x4E0U]) << '\n';
            return passed ? 0 : 2;
        }
        if (mode == "--replay-pending")
            std::cout << "pending_batch_replayed=" << injected
                      << " command_status=" << static_cast<unsigned>(ram[0x4E0U]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
