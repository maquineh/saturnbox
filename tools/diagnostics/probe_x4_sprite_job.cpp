// Replay the pending sprite job in the iteration-41 high-RAM snapshot.
// Usage: saturn-x4-sprite-probe ram_high.bin decoded.bin [--native]
// No game bytes or host decompression routine are embedded in this tool.
#include "saturn/core/memory_map.hpp"
#include "saturn/sh2/runtime.hpp"
#include "saturn/saturn/slave.hpp"
#include "saturn/sir/win64_jit.hpp"

#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 3 && (argc != 4 || std::string_view(argv[3]) != "--native")) return 2;
    const auto image = saturn::core::MemoryMap::fromFile(argv[1], 0x06000000U);
    if (!image || image->size() != 0x100000U) return 3;
    saturn::sir::MachineState state;
    for (const auto& [_, segment] : image->segments()) {
        for (std::size_t i = 0; i < segment.bytes.size(); ++i) {
            state.write8(segment.base + static_cast<std::uint32_t>(i),
                         std::to_integer<std::uint8_t>(segment.bytes[i]));
        }
    }
    const auto entry = state.read32BE(
        saturn::platform::BootRomServices::kDefaultVectorBase + 0x94U * 4U);
    const auto source = state.read32BE(0x060604D8U);
    const auto destination = state.read32BE(0x060604DCU);
    if (!entry || destination != 0x06003000U) return 4;
    saturn::platform::MmioInterceptor mmio;
    mmio.setAccessRecording(false);
    mmio.attachMachineState(state);
    saturn::platform::BootRomServices services;
    services.attachMmio(mmio);
    saturn::sir::Module module;
    services.install(module, state);
    state.setMemoryInterceptor(&services);
    state.writeState(4U, 0x94U);
    state.writeState(5U, entry);
    services.executeService(*services.resolve(services.kAipSetSh2DispatchStubAddress), state);
    if (!mmio.smpc().write(saturn::platform::SmpcRuntime::kComreg, 1U, 2U)) return 5;
    saturn::sh2::RuntimeBlockResolver resolver(*image, module, &state);
    const saturn::sir::BlockResolver resolve = [&](auto address) {
        if (auto* block = services.resolve(address)) return block;
        return resolver.resolve(address);
    };
    saturn::platform::SlaveSh2 slave;
    saturn::sir::Win64JitBlockExecutor jit;
    saturn::sir::BlockExecutionHook execute;
    if (argc == 4) execute = [&](const auto& block, auto& cpu) { return jit.execute(block, cpu); };
    for (std::size_t step = 0; step < 10000U; ++step) {
        if (step == 10U && !mmio.write(0x21000000U, 2U, 0xFFFFU)) return 6;
        if (!slave.tick(state, services, mmio, step, resolve, execute)) {
            std::cerr << "Slave failed at 0x" << std::hex << slave.pc() << '\n';
            return 7;
        }
        if (step > 10U && state.read32BE(0x060604D0U) == 0U) {
            std::ofstream output(argv[2], std::ios::binary);
            for (auto address = destination; address < destination + 0x780U; ++address) {
                output.put(static_cast<char>(state.read8(address)));
            }
            std::cout << "blocks=" << slave.executedBlocks()
                      << " native_blocks=" << jit.executedBlockCount() << " mailbox=0 source=0x"
                      << std::hex << source << " destination=0x" << destination
                      << " output_bytes=0x780\n";
            return output ? 0 : 8;
        }
    }
    std::cerr << "Pending job did not complete\n";
    return 9;
}
