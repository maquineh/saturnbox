#include "saturn/saturn/m68k.hpp"
#include "saturn/saturn/scsp_timing.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// Bounded offline driver execution. This bus preserves register writes for
// inspection. Timers/IRQs run on the sample clock; slots/DSP are not synthesized.
int main(int argc, char** argv) {
    const auto mode = argc == 5 ? std::string(argv[4]) : std::string{};
    if (argc < 2 || argc > 5 || (!mode.empty() && mode != "--no-timers" &&
                                mode != "--test-master-volume" && mode != "--replay-pending")) {
        std::cerr << "usage: saturn-scsp-probe <driver-or-sound-ram.bin> [cycles=1000000] [register-writes.csv] [--no-timers|--test-master-volume|--replay-pending]\n";
        return 1;
    }
    try {
        std::ifstream source(argv[1], std::ios::binary);
        if (!source) throw std::runtime_error("cannot open sound RAM input");
        std::vector<std::uint8_t> ram{std::istreambuf_iterator<char>(source), {}};
        if (ram.size() < 8U || ram.size() > 0x80000U)
            throw std::runtime_error("input must contain 8..524288 bytes");
        ram.resize(0x80000U);
        const auto budget = argc >= 3 ? std::stoull(argv[2]) : 1000000ULL;
        if (budget == 0U || budget > 1000000000ULL)
            throw std::runtime_error("cycle budget must be 1..1000000000");
        std::ofstream trace;
        if (argc >= 4) {
            trace.open(argv[3]);
            if (!trace) throw std::runtime_error("cannot open register trace output");
            trace << "cycles_before_batch,pc,address,width,value\n";
        }
        std::vector<std::uint8_t> registers(0x1000U);
        saturn::platform::ScspTiming timing;
        const bool timersEnabled = mode != "--no-timers";
        std::uint64_t registerWrites{}, slotWrites{}, keyOnRequests{}, unmappedReads{}, unmappedWrites{};
        saturn::platform::M68k* running{};
        const auto byteAt = [&](std::uint32_t address) -> std::uint8_t {
            address &= 0xFFFFFFU;
            if (address < ram.size()) return ram[address];
            if (address >= 0x100000U && address < 0x101000U) {
                if (timersEnabled) {
                    const auto timed = timing.read(static_cast<std::uint16_t>(address - 0x100000U), 1U);
                    if (timed) return static_cast<std::uint8_t>(*timed);
                }
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
            }
            if (address < 0x100400U && address >= 0x100000U && (address & 0x1FU) < 2U) {
                const auto slot = (address - 0x100000U) & ~0x1FU;
                const auto control = (registers[slot] << 8U) | registers[slot + 1U];
                if ((control & 0x1800U) == 0x1800U) ++keyOnRequests;
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
            if (timersEnabled) {
                sampleCycleRemainder += cycles;
                timing.advanceSamples(sampleCycleRemainder / 256U);
                sampleCycleRemainder %= 256U;
                cpu.setIrq(timing.irqLevel());
            }
        }
        if (trace.is_open()) {
            trace.flush();
            if (!trace) throw std::runtime_error("failed to write register trace");
        }
        std::cout << "reset_sp=0x" << std::hex << resetSp << " reset_pc=0x" << resetPc
                  << " final_pc=0x" << cpu.pc() << std::dec
                  << " cycles=" << cpu.stats().cyclesExecuted
                  << " register_writes=" << registerWrites << " slot_writes=" << slotWrites
                  << " slot_key_on_requests=" << keyOnRequests
                  << " unmapped_reads=" << unmappedReads << " unmapped_writes=" << unmappedWrites
                  << " timer_b_events=" << timing.stats().timers[1].expirations
                  << "\nSlot synthesis and DSP are not implemented in this probe.\n";
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
