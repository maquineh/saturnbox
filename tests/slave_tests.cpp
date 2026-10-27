#include "test_support.hpp"

#include "saturn/sh2/module_lifter.hpp"
#include "saturn/sh2/runtime.hpp"
#include "saturn/saturn/slave.hpp"
#include "saturn/sir/win64_jit.hpp"

namespace {

using namespace saturn;

void sharesCacheThroughWorkRam() {
    sir::MachineState state;
    platform::MmioInterceptor mmio;
    mmio.setAccessRecording(false);
    state.setMemoryInterceptor(&mmio);
    for (auto base : {0x00200000U, 0x06000000U}) {
        state.write32BE(base + 0x40U, 0x12345678U);
        REQUIRE(state.read32BE(base + 0x20000040U) == 0x12345678U);
        state.write16BE(base + 0x20000042U, 0xABCDU);
        REQUIRE(state.read32BE(base + 0x40U) == 0x1234ABCDU);
        REQUIRE(platform::classify(base) == platform::classify(base + 0x20000000U));
    }
    for (auto distinct : {0x40000000U, 0x60000000U, 0xC0000000U, 0xFFFFFE11U}) {
        REQUIRE(platform::canonicalizeRamAddress(distinct) == distinct);
    }
}

void runsSlaveCapturePollingWithPrivateRegisters() {
    // Poll FTCSR.ICF; on MINIT, acknowledge it and publish 42 through shared RAM.
    const auto image = test::imageFromWords(0x06010000U, {
        0xD30BU, 0xD40CU, 0x0009U, 0x6030U, 0xC880U, 0x89FCU,
        0xE000U, 0x2300U, 0xE12AU, 0x2412U, 0xAFF7U, 0x0009U,
        0x0009U, 0x0009U, 0x0009U, 0x0009U,
        0x0009U, 0x0009U, 0x0009U, 0x0009U,
        0x0009U, 0x0009U, 0x0009U, 0x0009U,
        0xFFFFU, 0xFE11U, 0x2600U, 0x3000U});
    for (bool native : {false, true}) {
        sir::Module module;
        sir::MachineState state;
        for (const auto& [_, segment] : image.segments()) {
            for (std::size_t i = 0; i < segment.bytes.size(); ++i) {
                state.write8(segment.base + static_cast<core::Address>(i),
                             std::to_integer<std::uint8_t>(segment.bytes[i]));
            }
        }
        platform::MmioInterceptor mmio;
        mmio.setAccessRecording(false);
        platform::BootRomServices services;
        services.attachMmio(mmio);
        services.install(module, state);
        state.setMemoryInterceptor(&services);
        state.writeState(4U, 0x94U);
        state.writeState(5U, image.base());
        services.executeService(*services.resolve(services.kAipSetSh2DispatchStubAddress), state);
        for (sir::StateSlot slot = 0; slot < 64U; ++slot) state.writeState(slot, slot + 0x1000U);
        const auto masterRegisters = state.stateSlots();
        REQUIRE(mmio.write(0xFFFFFE11U, 1U, 2U));
        sh2::RuntimeBlockResolver resolver(image, module, &state);
        sir::BlockResolver resolve = [&](auto address) { return resolver.resolve(address); };
        sir::Win64JitBlockExecutor jit;
        sir::BlockExecutionHook execute;
        if (native) execute = [&](const auto& block, auto& cpu) { return jit.execute(block, cpu); };
        platform::SlaveSh2 slave;
        REQUIRE(slave.tick(state, services, mmio, 0U, resolve, execute));
        REQUIRE(slave.executedBlocks() == 0U); // Reset at boot.
        REQUIRE(mmio.smpc().write(platform::SmpcRuntime::kComreg, 1U, 2U));
        REQUIRE(mmio.smpc().read(platform::SmpcRuntime::kOreg31, 1U) == 2U);
        for (std::size_t step = 1; step < 10; ++step) {
            REQUIRE(slave.tick(state, services, mmio, step, resolve, execute));
        }
        REQUIRE(state.read32BE(0x06003000U) == 0U);
        REQUIRE(mmio.write(0x21000000U, 2U, 0xFFFFU));
        REQUIRE(slave.tick(state, services, mmio, 10U, resolve, execute));
        // A polling service completes before the master's next block can
        // start a DMA from its output, independent of either CFG's splitting.
        REQUIRE(state.read32BE(0x06003000U) == 42U);
        for (std::size_t step = 10; step < 30; ++step) {
            REQUIRE(slave.tick(state, services, mmio, step, resolve, execute));
        }
        REQUIRE(state.read32BE(0x06003000U) == 42U);
        if (native && sir::Win64JitBlockExecutor::available()) {
            REQUIRE(jit.executedBlockCount() != 0U);
        }
        REQUIRE(mmio.read(0xFFFFFE11U, 1U) == 2U); // Slave flag did not alter master.
        REQUIRE(state.stateSlots() == masterRegisters);
        const auto blocks = slave.executedBlocks();
        REQUIRE(slave.tick(state, services, mmio, 29U, resolve, execute));
        REQUIRE(slave.executedBlocks() == blocks); // Nested interrupt boundary.
        REQUIRE(mmio.smpc().write(platform::SmpcRuntime::kComreg, 1U, 3U));
        REQUIRE(slave.tick(state, services, mmio, 30U, resolve, execute));
        REQUIRE(slave.executedBlocks() == blocks);
        state.write32BE(0x06003000U, 0U);
        REQUIRE(mmio.write(0x01000000U, 2U, 0xFFFFU));
        REQUIRE(slave.tick(state, services, mmio, 31U, resolve, execute));
        REQUIRE(mmio.smpc().write(platform::SmpcRuntime::kComreg, 1U, 2U));
        for (std::size_t step = 32; step < 42; ++step) {
            REQUIRE(slave.tick(state, services, mmio, step, resolve, execute));
        }
        REQUIRE(state.read32BE(0x06003000U) == 0U); // Reset discarded the old capture.
        REQUIRE(mmio.write(0x01000000U, 2U, 0xFFFFU));
        for (std::size_t step = 42; step < 62; ++step) {
            REQUIRE(slave.tick(state, services, mmio, step, resolve, execute));
        }
        REQUIRE(state.read32BE(0x06003000U) == 42U);
    }
}

void reportsUnresolvedSlaveEntryWithoutChangingMaster() {
    core::MemoryMap image;
    sir::Module module;
    sir::MachineState state;
    platform::MmioInterceptor mmio;
    platform::BootRomServices services;
    services.attachMmio(mmio);
    services.install(module, state);
    state.setMemoryInterceptor(&services);
    state.writeState(4U, 0x94U);
    state.writeState(5U, 0x06070000U);
    services.executeService(*services.resolve(services.kAipSetSh2DispatchStubAddress), state);
    const auto saved = state.stateSlots();
    REQUIRE(mmio.smpc().write(platform::SmpcRuntime::kComreg, 1U, 2U));
    sh2::RuntimeBlockResolver resolver(image, module, &state);
    platform::SlaveSh2 slave;
    REQUIRE(!slave.tick(state, services, mmio, 0U,
                       [&](auto address) { return resolver.resolve(address); }));
    REQUIRE(state.stateSlots() == saved);
    REQUIRE(state.memoryInterceptor() == &services);
    REQUIRE(slave.pc() == 0x06070000U);
}

} // namespace

void registerSlaveTests(test::Registry& registry) {
    registry.add("saturn: shares cache-through work RAM", sharesCacheThroughWorkRam);
    registry.add("saturn: runs slave FRT polling with private CPU registers",
                 runsSlaveCapturePollingWithPrivateRegisters);
    registry.add("saturn: reports unresolved slave entry without changing master",
                 reportsUnresolvedSlaveEntryWithoutChangingMaster);
}
