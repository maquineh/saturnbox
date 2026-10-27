#include "test_support.hpp"

#include "saturn/sh2/differential.hpp"
#include "saturn/sh2/module_lifter.hpp"
#include "saturn/sh2/runtime.hpp"
#include "saturn/saturn/boot_services.hpp"
#include "saturn/saturn/cd.hpp"
#include "saturn/saturn/mmio.hpp"
#include "saturn/saturn/scheduler.hpp"
#include "saturn/saturn/video.hpp"
#include "saturn/saturn/vdp1_compute.hpp"
#include "saturn/sir/interpreter.hpp"
#include "saturn/sir/module.hpp"
#include "saturn/sir/static_module.hpp"
#include "saturn/sir/win64_jit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace saturn;

sir::ExecutionResult executeStaticOne(const sir::Module&, sir::MachineState& state) {
    state.writeState(0U, 1U);
    sir::ExecutionResult result;
    result.slept = true;
    return result;
}

sir::ExecutionResult executeStaticTwo(const sir::Module&, sir::MachineState& state) {
    state.writeState(0U, 2U);
    sir::ExecutionResult result;
    result.slept = true;
    return result;
}

void cachesStaticDispatchUntilBlockBodyChanges() {
    sir::Module module;
    auto& block = module.createBlock(0x00220000U);
    sir::ModuleBuilder builder(module);
    const auto populate = [&](int value) {
        builder.setInsertPoint(block);
        builder.writeState(0U, builder.constI32(value));
        builder.sleep();
        block.markLowered();
    };
    populate(1);
    const auto first = sir::staticBlockSignature(block);
    const auto instructionCount = block.instructions().size();
    REQUIRE(module.resetBlockAt(block.address()) == &block);
    populate(2);
    const std::array entries{
        sir::StaticNativeBlockEntry{block.address(), first, executeStaticOne},
        sir::StaticNativeBlockEntry{block.address(), sir::staticBlockSignature(block),
                                   executeStaticTwo}};
    sir::StaticNativeBlockExecutor executor(module, entries);
    sir::MachineState actual;
    const auto check = [&](std::uint32_t expected) {
        sir::MachineState reference;
        const auto interpreted = sir::Interpreter{}.execute(block, reference);
        const auto compiled = executor.execute(block, actual);
        REQUIRE(compiled.has_value());
        REQUIRE(compiled->slept == interpreted.slept);
        REQUIRE(actual.readState(0U) == reference.readState(0U));
        REQUIRE(actual.readState(0U) == expected);
    };
    check(2U);
    check(2U); // cached dispatch
    REQUIRE(module.resetBlockAt(block.address()) == &block);
    populate(1); // same pointer and instruction count, different overlay
    REQUIRE(block.instructions().size() == instructionCount);
    check(1U);
    REQUIRE(module.resetBlockAt(block.address()) == &block);
    REQUIRE(!executor.execute(block, actual)); // cache a missing version
    populate(2); // appending must invalidate the cached miss, too
    check(2U);
    REQUIRE(executor.executedBlockCount() == 4U);
    REQUIRE(executor.overlayVariantSelections().size() == 2U);
    REQUIRE(executor.overlayVariantSelections()[0] == block.address());
    REQUIRE(executor.overlayVariantSelections()[1] == block.address());
}

void storesAndInterpretsConcreteSir() {
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    auto& taken = module.createBlock(0x108U);
    auto& notTaken = module.createBlock(0x104U);
    auto& function = module.createFunction(0x100U);
    function.setEntry(&entry);

    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    const auto original = builder.readState(0);
    const auto sum = builder.addI32(original, builder.constI32(2));
    builder.writeState(0, sum);
    builder.branchIf(builder.cmpEqI32(sum, builder.constI32(5)), &taken, &notTaken);

    REQUIRE(module.blockAt(0x100U) == &entry);
    REQUIRE(module.functionAt(0x100U) == &function);
    REQUIRE(entry.instructions().size() == 7);

    sir::MachineState state;
    state.writeState(0, 3);
    const auto result = sir::Interpreter{}.execute(entry, state);
    REQUIRE(result.status == sir::InterpreterStatus::Success);
    REQUIRE(result.control == sir::ControlKind::Branch);
    REQUIRE(result.target == &taken);
    REQUIRE(state.readState(0) == 5);
}

void win64JitMatchesInterpreterForPureSir() {
    if (!sir::Win64JitBlockExecutor::available()) return;

    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    auto& taken = module.createBlock(0x104U);
    auto& notTaken = module.createBlock(0x108U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    const auto left = builder.readState(1U);
    const auto right = builder.readState(2U);
    const auto sum = builder.addI32(left, right);
    const auto difference = builder.subI32(sum, builder.constI32(5));
    const auto product = builder.mulI32(difference, right);
    const auto shifted = builder.arithmeticShiftRightI32(product, builder.constI32(1));
    const auto mask = builder.andI32(shifted, builder.constI32(0xFFFF));
    const auto joined = builder.orI32(mask, builder.constI32(0x10000));
    const auto toggled = builder.xorI32(joined, builder.constI32(0x55));
    const auto inverted = builder.notI32(toggled);
    const auto logical = builder.logicalShiftRightI32(left, builder.constI32(3));
    const auto leftShift = builder.shiftLeftI32(right, builder.constI32(2));
    const auto extracted = builder.extractBitI32(left, 31U);
    const auto truncated8 = builder.truncateI32ToI8(left);
    const auto truncated16 = builder.truncateI32ToI16(left);
    const auto sign8 = builder.signExtendI8ToI32(left);
    const auto sign16 = builder.signExtendI16ToI32(left);
    const auto zero1 = builder.zeroExtendI1ToI32(left);
    const auto equal = builder.cmpEqI32(left, right);
    const auto signedGreaterEqual = builder.cmpSignedGreaterEqualI32(left, right);
    const auto unsignedGreaterEqual = builder.cmpUnsignedGreaterEqualI32(left, right);
    const auto signedGreater = builder.cmpSignedGreaterI32(left, right);
    const auto unsignedGreater = builder.cmpUnsignedGreaterI32(left, right);
    const auto boolXor = builder.xorI1(equal, unsignedGreater);
    builder.writeState(100U, difference);
    builder.writeState(101U, product);
    builder.writeState(102U, shifted);
    builder.writeState(103U, mask);
    builder.writeState(104U, joined);
    builder.writeState(105U, toggled);
    builder.writeState(106U, inverted);
    builder.writeState(107U, logical);
    builder.writeState(108U, leftShift);
    builder.writeState(109U, extracted);
    builder.writeState(110U, truncated8);
    builder.writeState(111U, truncated16);
    builder.writeState(112U, sign8);
    builder.writeState(113U, sign16);
    builder.writeState(114U, zero1);
    builder.writeState(115U, equal);
    builder.writeState(116U, signedGreaterEqual);
    builder.writeState(117U, unsignedGreaterEqual);
    builder.writeState(118U, signedGreater);
    builder.writeState(119U, unsignedGreater);
    builder.writeState(120U, boolXor);
    builder.branchIf(unsignedGreater, &taken, &notTaken);
    entry.markLowered();
    builder.setInsertPoint(taken);
    builder.trap(0x11U);
    taken.markLowered();
    builder.setInsertPoint(notTaken);
    builder.trap(0x22U);
    notTaken.markLowered();

    sir::MachineState initial;
    initial.writeState(1U, 0x80000081U);
    initial.writeState(2U, 10U);
    auto interpretedState = initial;
    const auto interpreted = sir::Interpreter{}.execute(entry, interpretedState);

    auto nativeState = initial;
    sir::Win64JitBlockExecutor jit;
    const auto native = jit.execute(entry, nativeState);
    REQUIRE(native.has_value());
    REQUIRE(jit.compiledBlockCount() == 1U);
    REQUIRE(native->status == interpreted.status);
    REQUIRE(native->control == interpreted.control);
    REQUIRE(native->target == interpreted.target);
    REQUIRE(native->dynamicTarget == interpreted.dynamicTarget);
    REQUIRE(nativeState == interpretedState);

    auto expectedProgramState = initial;
    const auto expectedProgram = sir::ProgramExecutor{}.run(&entry, expectedProgramState);
    auto nativeProgramState = initial;
    sir::ProgramExecutionOptions options;
    options.executeBlock = [&jit](const sir::BasicBlock& block,
                                  sir::MachineState& machineState) {
        return jit.execute(block, machineState);
    };
    const auto nativeProgram =
        sir::ProgramExecutor{}.run(&entry, nativeProgramState, options);
    REQUIRE(nativeProgram.status == expectedProgram.status);
    REQUIRE(nativeProgram.steps == expectedProgram.steps);
    REQUIRE(nativeProgram.last.trapVector == expectedProgram.last.trapVector);
    REQUIRE(nativeProgramState == expectedProgramState);
    REQUIRE(jit.compiledBlockCount() == 2U);
}

void win64JitMatchesInterpreterForRam() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    const auto address = builder.readState(1U);
    builder.store32(address, builder.constI32(0x8192A3B4));
    builder.writeState(2U, builder.signExtendI8ToI32(builder.load8(address)));
    const auto next = builder.addI32(address, builder.constI32(1));
    builder.writeState(3U, builder.signExtendI16ToI32(builder.load16(next)));
    builder.store16(next, builder.constI32(0x1234CDEF));
    builder.store8(builder.addI32(address, builder.constI32(3)), builder.constI32(0x12345678));
    builder.writeState(4U, builder.load32(address));
    builder.store32(builder.constI32(-2), builder.constI32(0x11223344));
    builder.writeState(5U, builder.load32(builder.constI32(-2)));
    builder.writeState(6U, builder.load32(builder.constI32(0x8000)));
    builder.ret(builder.constI32(0x200));
    entry.markLowered();

    sir::MachineState initial;
    initial.writeState(1U, 0x1001U);
    auto expected = initial;
    const auto interpreted = sir::Interpreter{}.execute(entry, expected);
    auto actual = initial;
    sir::Win64JitBlockExecutor jit;
    for (unsigned run = 0; run < 2; ++run) {
        actual = initial;
        const auto native = jit.execute(entry, actual);
        REQUIRE(native.has_value());
        REQUIRE(native->status == interpreted.status);
        REQUIRE(native->control == interpreted.control);
        REQUIRE(native->dynamicTarget == interpreted.dynamicTarget);
        REQUIRE(actual == expected);
        REQUIRE(actual.readState(2U) == 0xFFFFFF81U);
        REQUIRE(actual.readState(3U) == 0xFFFF92A3U);
        REQUIRE(actual.readState(4U) == 0x81CDEF78U);
        REQUIRE(actual.readState(5U) == 0x11223344U);
        REQUIRE(actual.readState(6U) == 0U);
        REQUIRE(actual.read8(0x1001U) == 0x81U);
        REQUIRE(actual.read8(0U) == 0x33U);
    }
    REQUIRE(jit.compiledBlockCount() == 1U);
}

void win64JitMatchesInterpreterForMmio() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    const auto address = builder.constI32(0x05E00000);
    builder.store32(address, builder.constI32(0x11223344));
    builder.writeState(4U, builder.load32(address));
    builder.store8(builder.constI32(0x05E00004), builder.constI32(0x123456AB));
    builder.writeState(5U, builder.load8(builder.constI32(0x05E00004)));
    builder.store16(builder.constI32(0x05E00006), builder.constI32(0x1234CDEF));
    builder.writeState(6U, builder.load16(builder.constI32(0x05E00006)));
    builder.trap(0x7FU);
    entry.markLowered();

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    state.setMemoryInterceptor(&mmio);
    platform::MmioInterceptor expectedMmio;
    sir::MachineState expected;
    expected.setMemoryInterceptor(&expectedMmio);
    const auto interpreted = sir::ProgramExecutor{}.run(&entry, expected);
    sir::Win64JitBlockExecutor jit;

    sir::ProgramExecutionOptions options;
    options.executeBlock = [&jit](const sir::BasicBlock& block,
                                  sir::MachineState& machineState) {
        return jit.execute(block, machineState);
    };
    const auto execution = sir::ProgramExecutor{}.run(&entry, state, options);
    REQUIRE(execution.status == sir::ProgramStatus::Trap);
    REQUIRE(execution.last.trapVector == 0x7FU);
    REQUIRE(execution.status == interpreted.status);
    REQUIRE(execution.steps == interpreted.steps);
    REQUIRE(state == expected);
    REQUIRE(jit.compiledBlockCount() == 1U);
    REQUIRE(state.readState(4U) == 0x11223344U);
    REQUIRE(state.memory().empty());
    REQUIRE(mmio.accesses().size() == 6U);
    REQUIRE(mmio.accesses().size() == expectedMmio.accesses().size());
    for (std::size_t index = 0; index < mmio.accesses().size(); ++index) {
        const auto& actual = mmio.accesses()[index];
        const auto& reference = expectedMmio.accesses()[index];
        REQUIRE(actual.address == reference.address);
        REQUIRE(actual.width == reference.width);
        REQUIRE(actual.value == reference.value);
        REQUIRE(actual.write == reference.write);
        REQUIRE(actual.space == reference.space);
        REQUIRE(actual.context.blockAddress == reference.context.blockAddress);
        REQUIRE(actual.context.step == reference.context.step);
        REQUIRE(actual.context.state == reference.context.state);
    }
}

void win64JitPreservesLiveStateAroundMemory() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    struct Interceptor final : sir::MemoryInterceptor {
        sir::MachineState* state{};
        std::vector<std::array<std::uint32_t, 4>> accesses;
        std::optional<std::uint32_t> read(core::Address address, std::uint8_t width) override {
            accesses.push_back({address, width, state->readState(1U), 0U});
            if (address != 0x05E00000U) return std::nullopt;
            state->writeState(1U, 33U);
            return 0x1234FEDCU;
        }
        bool write(core::Address address, std::uint8_t width, std::uint32_t value) override {
            accesses.push_back({address, width, state->readState(1U), value});
            if (address != 0x05E00000U) return false;
            state->writeState(1U, state->readState(1U) + 1U);
            return true;
        }
    };
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.writeState(1U, builder.constI32(7));
    const auto mmioAddress = builder.constI32(0x05E00000);
    builder.store8(mmioAddress, builder.constI32(0x123456AB));
    builder.writeState(2U, builder.readState(1U));
    builder.writeState(3U, builder.load16(mmioAddress));
    builder.writeState(4U, builder.readState(1U));
    const auto ramAddress = builder.constI32(0x1000);
    builder.store32(ramAddress, builder.constI32(0x11223344));
    builder.writeState(5U, builder.load32(ramAddress));
    builder.trap(1U);
    entry.markLowered();

    sir::MachineState expected, actual;
    Interceptor reference, interceptor;
    reference.state = &expected;
    interceptor.state = &actual;
    expected.setMemoryInterceptor(&reference);
    actual.setMemoryInterceptor(&interceptor);
    REQUIRE(sir::Interpreter{}.execute(entry, expected));
    sir::Win64JitBlockExecutor jit;
    REQUIRE(jit.execute(entry, actual).has_value());
    REQUIRE(actual == expected);
    REQUIRE(interceptor.accesses == reference.accesses);
    REQUIRE(actual.readState(2U) == 8U);
    REQUIRE(actual.readState(3U) == 0xFEDCU);
    REQUIRE(actual.readState(4U) == 33U);
    REQUIRE(actual.readState(5U) == 0x11223344U);
    REQUIRE(interceptor.accesses[0][3] == 0xABU);
}

void win64JitPropagatesMemoryExceptions() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    struct Interceptor final : sir::MemoryInterceptor {
        bool fail{true};
        unsigned calls{};
        std::optional<std::uint32_t> read(core::Address, std::uint8_t) override {
            ++calls;
            if (fail) throw std::runtime_error("memory failure");
            return 5U;
        }
        bool write(core::Address address, std::uint8_t, std::uint32_t) override {
            ++calls;
            if (address != 0x05E00000U) return false;
            if (fail) throw std::runtime_error("memory failure");
            return true;
        }
    };
    for (const bool failingRead : {false, true}) {
        sir::Module module;
        auto& entry = module.createBlock(0x100U);
        sir::ModuleBuilder builder(module);
        builder.setInsertPoint(entry);
        builder.writeState(1U, builder.constI32(7));
        builder.store32(builder.constI32(0x1000), builder.constI32(0x11223344));
        const auto address = builder.constI32(0x05E00000);
        if (failingRead) builder.writeState(2U, builder.load32(address));
        else builder.store32(address, builder.constI32(9));
        builder.writeState(1U, builder.constI32(99));
        builder.trap(1U);
        entry.markLowered();
        sir::MachineState actual, expected;
        Interceptor interceptor, reference;
        actual.setMemoryInterceptor(&interceptor);
        expected.setMemoryInterceptor(&reference);
        sir::Win64JitBlockExecutor jit;
        bool nativeThrew = false, interpreterThrew = false;
        try { (void)jit.execute(entry, actual); }
        catch (const std::runtime_error& error) {
            nativeThrew = std::string(error.what()) == "memory failure";
        }
        try { (void)sir::Interpreter{}.execute(entry, expected); }
        catch (const std::runtime_error& error) {
            interpreterThrew = std::string(error.what()) == "memory failure";
        }
        REQUIRE(nativeThrew && interpreterThrew);
        REQUIRE(actual == expected);
        REQUIRE(actual.readState(1U) == 7U);
        REQUIRE(actual.memory().size() == 4U);
        REQUIRE(interceptor.calls == reference.calls);
        REQUIRE(interceptor.calls == 2U);
        interceptor.fail = false;
        reference.fail = false;
        REQUIRE(jit.execute(entry, actual).has_value());
        REQUIRE(sir::Interpreter{}.execute(entry, expected));
        REQUIRE(actual == expected);
        REQUIRE(actual.readState(1U) == 99U);
        REQUIRE(jit.compiledBlockCount() == 1U);
    }
}

void win64JitFallsBackBeforeMemoryEffects() {
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.store32(builder.constI32(0x05E00000), builder.constI32(0x11223344));
    const auto sum = builder.addWithCarryI32(builder.constI32(1), builder.constI32(2),
                                            builder.constI1(true));
    builder.writeState(4U, sum.value);
    builder.trap(0x7FU);
    // An unfinished lowering must not execute a partially compiled block.
    platform::MmioInterceptor mmio;
    sir::MachineState state;
    state.setMemoryInterceptor(&mmio);
    sir::Win64JitBlockExecutor jit;
    REQUIRE(!jit.execute(entry, state).has_value());
    REQUIRE(!jit.execute(entry, state).has_value());
    REQUIRE(mmio.accesses().empty());
    REQUIRE(state.stateSlots().empty());
    sir::ProgramExecutionOptions options;
    options.executeBlock = [&jit](const sir::BasicBlock& block, sir::MachineState& machine) {
        return jit.execute(block, machine);
    };
    REQUIRE(sir::ProgramExecutor{}.run(&entry, state, options).status == sir::ProgramStatus::Trap);
    REQUIRE(state.readState(4U) == 4U);
    REQUIRE(mmio.accesses().size() == 1U);
    REQUIRE(jit.compiledBlockCount() == 0U);
}

void comparesDelayedConditionalBlock() {
    // MOV #5,R0; ADD #3,R0; CMP/EQ #8,R0; BT/S 0x10E;
    // ADD #1,R0; RTS; NOP; RTS; NOP.
    const auto image = test::imageFromWords(0x100U, {
        0xE005U, 0x7003U, 0x8808U, 0x8D02U, 0x7001U,
        0x000BU, 0x0009U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    sir::Module module;
    const auto report = sh2::lowerCFG(cfg, module);
    REQUIRE(report.success());
    REQUIRE(report.loweredBlocks == cfg.blocks.size());

    const auto* source = cfg.blockAt(0x100U);
    const auto* lowered = module.blockAt(0x100U);
    REQUIRE(source != nullptr && lowered != nullptr);
    const auto comparison = sh2::compareBlock(*source, *lowered, {});
    REQUIRE(comparison.matches());
    REQUIRE(comparison.sirControl.target == 0x10EU);

    sir::MachineState state;
    const auto execution = sir::Interpreter{}.execute(*lowered, state);
    REQUIRE(execution);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R0)) == 9U);
    REQUIRE(state.readState(sh2::kStateSlotT) == 1U);
}

void comparesMemoryAndReturnBlock() {
    // MOV.L @R4+,R2; ADD R2,R1; RTS; MOV.L R1,@R5.
    const auto image = test::imageFromWords(0x200U, {
        0x6246U, 0x312CU, 0x000BU, 0x2512U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x200U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());
    const auto* source = cfg.blockAt(0x200U);
    const auto* lowered = module.blockAt(0x200U);
    REQUIRE(source != nullptr && lowered != nullptr);

    sir::MachineState initial;
    initial.writeState(sh2::stateSlot(sh2::Register::R1), 2U);
    initial.writeState(sh2::stateSlot(sh2::Register::R4), 0x1000U);
    initial.writeState(sh2::stateSlot(sh2::Register::R5), 0x2000U);
    initial.writeState(sh2::stateSlot(sh2::Register::PR), 0x3000U);
    initial.write32BE(0x1000U, 7U);

    const auto comparison = sh2::compareBlock(*source, *lowered, initial);
    REQUIRE(comparison.matches());
    REQUIRE(comparison.sirControl.kind == sir::ControlKind::Return);
    REQUIRE(comparison.sirControl.target == 0x3000U);

    auto interpreted = initial;
    const auto execution = sir::Interpreter{}.execute(*lowered, interpreted);
    REQUIRE(execution);
    REQUIRE(interpreted.readState(sh2::stateSlot(sh2::Register::R1)) == 9U);
    REQUIRE(interpreted.readState(sh2::stateSlot(sh2::Register::R2)) == 7U);
    REQUIRE(interpreted.readState(sh2::stateSlot(sh2::Register::R4)) == 0x1004U);
    REQUIRE(interpreted.read32BE(0x2000U) == 9U);
    if (sir::Win64JitBlockExecutor::available()) {
        auto nativeState = initial;
        sir::Win64JitBlockExecutor jit;
        const auto native = jit.execute(*lowered, nativeState);
        REQUIRE(native.has_value());
        REQUIRE(native->control == execution.control);
        REQUIRE(native->dynamicTarget == execution.dynamicTarget);
        REQUIRE(nativeState == interpreted);
    }
}

void comparesCarryShiftAndDivision() {
    // MOV #-1,R1; MOV #1,R2; ADDC R2,R1; SHLL R1; DIV0U;
    // DIV1 R2,R1; RTS; NOP.
    const auto image = test::imageFromWords(0x400U, {
        0xE1FFU, 0xE201U, 0x312EU, 0x4100U,
        0x0019U, 0x3124U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x400U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());
    const auto* source = cfg.blockAt(0x400U);
    const auto* lowered = module.blockAt(0x400U);
    REQUIRE(source != nullptr && lowered != nullptr);

    sir::MachineState initial;
    initial.writeState(sh2::stateSlot(sh2::Register::PR), 0x800U);
    const auto comparison = sh2::compareBlock(*source, *lowered, initial);
    REQUIRE(comparison.matches());
}

void comparesStringCompare() {
    // CMP/STR R4,R6; RTS; NOP. CMP/STR sets T when any corresponding byte
    // in the two registers is equal.
    const auto image = test::imageFromWords(0x500U, {
        0x264CU, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x500U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());
    const auto* source = cfg.blockAt(0x500U);
    const auto* lowered = module.blockAt(0x500U);
    REQUIRE(source != nullptr && lowered != nullptr);

    sir::MachineState initial;
    initial.writeState(sh2::stateSlot(sh2::Register::R4), 0x11223344U);
    initial.writeState(sh2::stateSlot(sh2::Register::R6), 0xAABB33CCU);
    initial.writeState(sh2::stateSlot(sh2::Register::PR), 0x700U);
    const auto comparison = sh2::compareBlock(*source, *lowered, initial);
    REQUIRE(comparison.matches());

    auto interpreted = initial;
    REQUIRE(sir::Interpreter{}.execute(*lowered, interpreted));
    REQUIRE(interpreted.readState(sh2::kStateSlotT) == 1U);
}

void runsMultipleBlocksWithStepLimit() {
    // ADD #1,R0; BRA 0x100; NOP.
    const auto image = test::imageFromWords(0x100U, {0x7001U, 0xAFFDU, 0x0009U});
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());

    sir::MachineState state;
    sir::ProgramExecutionOptions options;
    options.maxSteps = 3;
    const auto result = sir::ProgramExecutor{}.run(module.blockAt(0x100U), state, options);
    REQUIRE(result.status == sir::ProgramStatus::StepLimit);
    REQUIRE(result.steps == 3);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R0)) == 3U);
}

void observesBlockEntries() {
    // MOV #1,R0; BRA 0x100; NOP.
    const auto image = test::imageFromWords(0x100U, {0xE001U, 0xAFFDU, 0x0009U});
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());

    std::vector<core::Address> trace;
    sir::ProgramExecutionOptions options;
    options.maxSteps = 2;
    options.observeBlock = [&trace](const sir::BasicBlock& block, std::size_t) {
        trace.push_back(block.address());
    };
    sir::MachineState state;
    const auto result = sir::ProgramExecutor{}.run(module.blockAt(0x100U),
                                                    state, options);
    REQUIRE(result.status == sir::ProgramStatus::StepLimit);
    const std::vector<core::Address> expected{0x100U, 0x100U};
    REQUIRE(trace == expected);
}

void installsBootServiceTable() {
    sir::Module module;
    sir::MachineState state;
    platform::BootRomServices services;
    services.install(module, state);

    REQUIRE(services.slots().size() == 13);
    for (const auto& slot : services.slots()) {
        REQUIRE(state.read32BE(slot.pointerAddress) == slot.targetAddress);
        REQUIRE(services.resolve(slot.targetAddress) != nullptr);
        REQUIRE(services.resolve(slot.targetAddress)->isLowered());
    }
    REQUIRE(services.resolve(platform::BootRomServices::kStubAddress) != nullptr);
    REQUIRE(services.resolve(platform::BootRomServices::kStubAddress)->isLowered());
    REQUIRE(services.resolve(platform::BootRomServices::kExitStubAddress) != nullptr);
    REQUIRE(services.resolve(platform::BootRomServices::kExitStubAddress)->isLowered());
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R15)) ==
            platform::BootRomServices::kDefaultMasterStack);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::VBR)) ==
            platform::BootRomServices::kDefaultVectorBase);

    const auto first = services.read(platform::BootRomServices::kAipSyncWord, 2);
    const auto second = services.read(platform::BootRomServices::kAipSyncWord, 2);
    REQUIRE(first == 1U);
    REQUIRE(second == 2U);

    const auto exit = sir::Interpreter{}.execute(
        *services.resolve(platform::BootRomServices::kExitStubAddress), state);
    REQUIRE(exit.status == sir::InterpreterStatus::Success);
    REQUIRE(exit.slept);

    const auto execution = sir::ProgramExecutor{}.run(
        services.resolve(platform::BootRomServices::kExitStubAddress), state);
    REQUIRE(execution.status == sir::ProgramStatus::Sleeping);
}

void executesBootBupInitSemantics() {
    sir::Module module;
    sir::MachineState state;
    platform::BootRomServices services;
    services.install(module, state);
    state.setMemoryInterceptor(&services);

    constexpr core::Address table = 0x06006000U;
    constexpr core::Address devices = 0x06002068U;
    state.writeState(sh2::stateSlot(sh2::Register::R5), table);
    state.writeState(sh2::stateSlot(sh2::Register::R6), devices);
    services.executeService(
        *services.resolve(platform::BootRomServices::kBupInitStubAddress), state);

    REQUIRE(state.read32BE(platform::BootRomServices::kBupInitPointer) ==
            platform::BootRomServices::kBupInitStubAddress);
    REQUIRE(state.read32BE(table) == platform::BootRomServices::kBupFunctionTableFirst);
    REQUIRE(state.read32BE(table + 4U * 11U) ==
            platform::BootRomServices::kBupFunctionTableLast);
    REQUIRE(state.read16BE(devices) == 1U);
    REQUIRE(state.read16BE(devices + 2U) == 1U);
    REQUIRE(state.read16BE(devices + 4U) == 0U);
    REQUIRE(state.read16BE(devices + 6U) == 0U);
    REQUIRE(state.read16BE(devices + 8U) == 0U);
    REQUIRE(state.read16BE(devices + 10U) == 0U);

    state.writeState(sh2::stateSlot(sh2::Register::R0), 0xFFFFFFFFU);
    services.executeService(*services.resolve(platform::BootRomServices::kBupFunctionTableFirst + 4U),
                            state);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R0)) == 0U);
}

void continuesAfterExecuteDmpWhenEnabled() {
    sir::Module module;
    sir::MachineState state;
    platform::BootRomServices services;
    services.setExecuteDmpReturns(true);
    services.install(module, state);

    state.writeState(sh2::stateSlot(sh2::Register::PR), 0x0601D096U);
    const auto result = sir::Interpreter{}.execute(
        *services.resolve(platform::BootRomServices::kExitStubAddress), state);
    REQUIRE(result.status == sir::InterpreterStatus::Success);
    REQUIRE(result.control == sir::ControlKind::Return);
    REQUIRE(!result.slept);
    REQUIRE(result.dynamicTarget == 0x0601D096U);
}

void tracesBootServiceCallerAndRegisters() {
    sir::Module module;
    sir::MachineState state;
    platform::BootRomServices services;
    services.install(module, state);
    state.setMemoryInterceptor(&services);

    auto& caller = module.createBlock(0x100U, "caller");
    auto& returned = module.createBlock(0x200U, "returned");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(caller);
    builder.writeState(sh2::stateSlot(sh2::Register::R3), builder.constI32(0xCAFE));
    builder.writeState(sh2::stateSlot(sh2::Register::PR), builder.constI32(0x200));
    builder.callIndirect(builder.constI32(platform::BootRomServices::kCdInit1StubAddress),
                         &returned);
    builder.setInsertPoint(returned);
    builder.sleep();

    sir::ProgramExecutionOptions options;
    options.resolveBlock = [&services, &module](core::Address address) {
        if (auto* service = services.resolve(address)) {
            return service;
        }
        return module.blockAt(address);
    };
    const auto execution = sir::ProgramExecutor{}.run(&caller, state, options);
    REQUIRE(execution.status == sir::ProgramStatus::Sleeping);
    REQUIRE(services.calls().size() == 1);
    REQUIRE(services.calls()[0].service.name == "SYS_CDINIT1");
    REQUIRE(services.calls()[0].context.blockAddress ==
            platform::BootRomServices::kCdInit1StubAddress);
    REQUIRE(services.calls()[0].context.callerBlock == 0x100U);
    REQUIRE(services.calls()[0].context.state.at(
                sh2::stateSlot(sh2::Register::R3)) == 0xCAFEU);
}

void executesBootDispatchSemantics() {
    sir::Module module;
    sir::MachineState state;
    platform::MmioInterceptor mmio;
    platform::BootRomServices services;
    services.attachMmio(mmio);
    services.install(module, state);
    state.setMemoryInterceptor(&services);

    const auto r4 = sh2::stateSlot(sh2::Register::R4);
    const auto r5 = sh2::stateSlot(sh2::Register::R5);
    const auto r0 = sh2::stateSlot(sh2::Register::R0);
    state.writeState(r0, 0xCAFEBABEU);
    state.writeState(r4, 0x40U);
    state.writeState(r5, 0x06012345U);
    services.executeService(*services.resolve(platform::BootRomServices::kAipDispatchStubAddress),
                             state);
    REQUIRE(state.read32BE(platform::BootRomServices::kScuInterruptTable + 0x40U * 4U) ==
            0x06012345U);
    REQUIRE(state.readState(r0) == 0xCAFEBABEU);

    state.writeState(r4, 0x40U);
    services.executeService(*services.resolve(platform::BootRomServices::kAipGetDispatchStubAddress),
                             state);
    REQUIRE(state.readState(r0) == 0x06012345U);

    state.writeState(r4, 0x40U);
    state.writeState(r5, 0x06023456U);
    services.executeService(*services.resolve(platform::BootRomServices::kAipSetSh2DispatchStubAddress),
                             state);
    services.executeService(*services.resolve(platform::BootRomServices::kAipGetSh2DispatchStubAddress),
                             state);
    REQUIRE(state.readState(r0) == 0x06023456U);

    state.write32BE(platform::BootRomServices::kScuInterruptMaskStorage, 0xFFFF0000U);
    state.writeState(r4, 0x0F0F7FFFU);
    state.writeState(r5, 0x00000005U);
    services.executeService(*services.resolve(platform::BootRomServices::kAipCdDispatchStubAddress),
                             state);
    REQUIRE(state.read32BE(platform::BootRomServices::kScuInterruptMaskStorage) ==
            0x0F0F0005U);
    REQUIRE(mmio.read(platform::BootRomServices::kScuInterruptMaskRegister, 4) ==
            0x0F0F0005U);
    REQUIRE(mmio.read(platform::BootRomServices::kScuInterruptStatusRegister, 4) == 0U);
    REQUIRE(mmio.read(platform::BootRomServices::kAbusInterruptAcknowledge, 4) == 1U);

    state.writeState(r4, 1U);
    services.executeService(
        *services.resolve(platform::BootRomServices::kAipChangeSystemClockStubAddress),
        state);
    REQUIRE(state.read32BE(platform::BootRomServices::kSystemClockStorage) == 1U);

    state.writeState(r4, 3U);
    services.executeService(
        *services.resolve(platform::BootRomServices::kAipTasSemaphoreStubAddress),
        state);
    REQUIRE(state.readState(r0) == 1U);
    REQUIRE(state.read8(platform::BootRomServices::kSemaphoreBase + 3U) == 0x80U);
    services.executeService(
        *services.resolve(platform::BootRomServices::kAipTasSemaphoreStubAddress),
        state);
    REQUIRE(state.readState(r0) == 0U);
    services.executeService(
        *services.resolve(platform::BootRomServices::kAipClearSemaphoreStubAddress),
        state);
    REQUIRE(state.read8(platform::BootRomServices::kSemaphoreBase + 3U) == 0U);

    state.writeState(r4, 0x0000F0F0U);
    services.executeService(
        *services.resolve(platform::BootRomServices::kAipSetScuMaskStubAddress),
        state);
    REQUIRE(state.read32BE(platform::BootRomServices::kScuInterruptMaskStorage) ==
            0x0000F0F0U);
    REQUIRE(mmio.read(platform::BootRomServices::kScuInterruptMaskRegister, 4) ==
            0x0000F0F0U);
    REQUIRE(mmio.read(platform::BootRomServices::kScuInterruptStatusRegister, 4) == 0U);
}

void executesSmpcIntbackWithConfiguredPad() {
    platform::MmioInterceptor mmio;
    platform::DigitalPadState pad;
    pad.set(platform::PadButton::Start);
    pad.set(platform::PadButton::A);
    mmio.smpc().setPad(0, pad);

    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg1, 1, 0x08U));
    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg2, 1, 0xF0U));
    REQUIRE(mmio.write(platform::SmpcRuntime::kStatusFlag, 1, 1U));
    REQUIRE(mmio.write(platform::SmpcRuntime::kComreg, 1,
                       platform::SmpcRuntime::kIntback));

    REQUIRE(mmio.read(platform::SmpcRuntime::kStatusFlag, 1) == 0U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kStatus, 1) == 0xC0U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0, 1) == 0xF1U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 2U, 1) == 0x02U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 4U, 1) == 0xF3U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 6U, 1) == 0xFFU);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 8U, 1) == 0xF0U);

    // 255-byte mode for both ports still returns a first packet, with
    // the mode echo in SR's low nibble (PDL must not be cleared).
    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg1, 1, 0x58U));
    REQUIRE(mmio.write(platform::SmpcRuntime::kComreg, 1,
                       platform::SmpcRuntime::kIntback));
    REQUIRE(mmio.read(platform::SmpcRuntime::kStatus, 1) == 0xC5U);

    REQUIRE(mmio.smpc().takeInterruptRequest());
    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg0, 1, 1U));
    REQUIRE(mmio.write(platform::SmpcRuntime::kComreg, 1,
                       platform::SmpcRuntime::kIntback));
    REQUIRE(mmio.read(platform::SmpcRuntime::kStatus, 1) == 0x40U);
    REQUIRE(mmio.smpc().takeInterruptRequest());
    mmio.smpc().onVBlank();
    REQUIRE(!mmio.smpc().takeInterruptRequest());
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0, 1) != 0xF1U);
    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg0, 1, 0x80U));
    REQUIRE(mmio.read(platform::SmpcRuntime::kStatus, 1) == 0xC5U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 4U, 1) == 0xF3U);
    REQUIRE(mmio.smpc().takeInterruptRequest());
}

void refreshesSmpcIntbackAtVblank() {
    platform::MmioInterceptor mmio;
    platform::DigitalPadState pad;
    pad.set(platform::PadButton::A);
    mmio.smpc().setPad(0, pad);
    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg1, 1, 0x08U));
    REQUIRE(mmio.write(platform::SmpcRuntime::kComreg, 1,
                       platform::SmpcRuntime::kIntback));
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 4U, 1) == 0xFBU);

    mmio.smpc().setPad(0, platform::DigitalPadState{});
    mmio.smpc().onVBlank();
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 4U, 1) == 0xFFU);

    sir::Module module;
    auto& block = module.createBlock(0x100U);
    sir::MachineState state;
    platform::ScuEventScheduler scheduler;
    REQUIRE(mmio.write(0x25FE00A0U, 4U, 0xFFFFFFFFU));
    REQUIRE(!scheduler.beforeBlock(block, state, 0U, mmio));
    REQUIRE((mmio.scuInterruptStatus() & 0x80U) != 0U);
    REQUIRE(!mmio.smpc().takeInterruptRequest());
    mmio.clearScuInterrupt(0x80U);
    REQUIRE(mmio.write(platform::SmpcRuntime::kIreg0, 1, 0x40U));
    mmio.smpc().setPad(0, pad);
    mmio.smpc().onVBlank();
    REQUIRE(!scheduler.beforeBlock(block, state, 1U, mmio));
    REQUIRE((mmio.scuInterruptStatus() & 0x80U) == 0U);
    REQUIRE(mmio.read(platform::SmpcRuntime::kOreg0 + 4U, 1) == 0xFFU);
}

void executesCdImageBackedFifo() {
    platform::MmioInterceptor mmio;
    std::array<std::byte, 4> image{};
    image[0] = std::byte{0x12};
    image[1] = std::byte{0x34};
    image[2] = std::byte{0x56};
    image[3] = std::byte{0x78};
    REQUIRE(mmio.cd().setImage(image, 4U));
    REQUIRE(mmio.cd().hasImage());
    REQUIRE(mmio.cd().sectorCount() == 1U);

    const auto regs = platform::CdRuntime::kCommandBase;
    REQUIRE(mmio.write(regs + 0x18U, 2, 0x3000U));
    REQUIRE(mmio.write(regs + 0x1CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x20U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x24U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x18U, 2, 0x1080U));
    REQUIRE(mmio.write(regs + 0x1CU, 2, 150U));
    REQUIRE(mmio.write(regs + 0x20U, 2, 0x0080U));
    REQUIRE(mmio.write(regs + 0x24U, 2, 1U));
    mmio.cd().onSubcodeTick(1U);
    REQUIRE(mmio.write(regs + 0x18U, 2, 0x6100U));
    REQUIRE(mmio.write(regs + 0x1CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x20U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x24U, 2, 1U));
    REQUIRE((mmio.cd().hirq() & platform::CdRuntime::kHirqCsct) != 0U);
    REQUIRE(mmio.read(platform::CdRuntime::kWordFifo + 0x20000000U, 2) ==
            0x1234U);
    REQUIRE(mmio.read(platform::CdRuntime::kWordFifo + 0x20000000U, 2) ==
            0x5678U);
    // Draining the FIFO ends the host transfer (DRDY), but the sector remains
    // retained until DELETE SECTOR DATA (0x62) or selector reset.
    REQUIRE((mmio.cd().hirq() & platform::CdRuntime::kHirqCsct) != 0U);

    platform::CdRuntime rawCd;
    std::vector<std::byte> rawSector(2448U);
    rawSector[16] = std::byte{0x9A};
    rawSector[17] = std::byte{0xBC};
    rawSector[18] = std::byte{0xDE};
    rawSector[19] = std::byte{0xF0};
    REQUIRE(rawCd.setImage(std::move(rawSector), 2448U));
    rawCd.prepareSector(0U);
    REQUIRE(rawCd.read(platform::CdRuntime::kFifo, 4) == 0x9ABCDEF0U);
}

void transfersCdFifoThroughScuDma() {
    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.setMemoryInterceptor(&mmio);

    std::vector<std::byte> image(platform::CdRuntime::kLogicalSectorSize);
    image[0] = std::byte{0xDE};
    image[1] = std::byte{0xAD};
    image[2] = std::byte{0xBE};
    image[3] = std::byte{0xEF};
    REQUIRE(mmio.cd().setImage(std::move(image)));

    const auto command = platform::CdRuntime::kCommandBase;
    REQUIRE(mmio.write(command + 0x18U, 2, 0x3000U));
    REQUIRE(mmio.write(command + 0x1CU, 2, 0U));
    REQUIRE(mmio.write(command + 0x20U, 2, 0U));
    REQUIRE(mmio.write(command + 0x24U, 2, 0U));
    REQUIRE(mmio.write(command + 0x18U, 2, 0x1080U));
    REQUIRE(mmio.write(command + 0x1CU, 2, 150U));
    REQUIRE(mmio.write(command + 0x20U, 2, 0x0080U));
    REQUIRE(mmio.write(command + 0x24U, 2, 1U));

    constexpr core::Address destination = 0x06004000U;
    const auto dma = platform::ScuDmaRuntime::kDma0;
    REQUIRE(mmio.write(dma + 0x00U, 4, platform::CdRuntime::kFifo));
    REQUIRE(mmio.write(dma + 0x04U, 4, destination));
    REQUIRE(mmio.write(dma + 0x08U, 4, 4U)); // one 32-bit word (byte count)
    REQUIRE(mmio.write(dma + 0x0CU, 4, 4U));
    REQUIRE(mmio.write(dma + 0x14U, 4, 0x107U)); // direct, WUP, manual GO
    REQUIRE(mmio.write(dma + 0x10U, 4, 0x100U)); // arm permission before CD data

    mmio.cd().onSubcodeTick(1U);
    REQUIRE(mmio.write(command + 0x18U, 2, 0x6100U));
    REQUIRE(mmio.write(command + 0x1CU, 2, 0U));
    REQUIRE(mmio.write(command + 0x20U, 2, 0U));
    REQUIRE(mmio.write(command + 0x24U, 2, 1U));
    // The game acknowledges DRDY before issuing the manual DMA GO.  The
    // payload remains readable in the host FIFO after that interrupt latch is
    // cleared, so DMA must key off FIFO occupancy rather than HIRQ DRDY.
    REQUIRE(mmio.write(command + 0x08U, 2, 0xFFFDU));
    REQUIRE(mmio.write(dma + 0x10U, 4, 0x101U));
    mmio.scuDma().onCdStateChanged();

    REQUIRE(state.read32BE(destination) == 0xDEADBEEFU);
    REQUIRE(mmio.scuDma().lastTransfer()->complete);
    REQUIRE(mmio.scuDma().lastTransfer()->fromCd);
    REQUIRE(mmio.scuDma().lastTransfer()->bytes == 4U);
    REQUIRE(mmio.read(dma + 0x10U, 4) == 0x100U);
    REQUIRE(mmio.write(dma + 0x10U, 4, 0U));
    REQUIRE(!mmio.scuDma().descriptors()[0].enabled);
}

void triggersScuDmaAtVblank() {
    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.setMemoryInterceptor(&mmio);

    constexpr core::Address source = 0x06005000U;
    constexpr core::Address destination = 0x06006000U;
    state.write32BE(source, 0x12345678U);
    const auto dma = platform::ScuDmaRuntime::kDma0;
    REQUIRE(mmio.write(dma + 0x00U, 4, source));
    REQUIRE(mmio.write(dma + 0x04U, 4, destination));
    REQUIRE(mmio.write(dma + 0x08U, 4, 4U));
    REQUIRE(mmio.write(dma + 0x0CU, 4, 0x101U)); // R4/W2
    REQUIRE(mmio.write(dma + 0x14U, 4, 0x00000000U)); // VBlank-IN factor
    REQUIRE(mmio.write(dma + 0x10U, 4, 0x00000100U)); // permission only
    REQUIRE(state.read32BE(destination) == 0U);

    mmio.scuDma().onVBlank();
    REQUIRE(state.read32BE(destination) == 0x12345678U);
    REQUIRE(mmio.scuDma().lastTransfer().has_value());
    REQUIRE(mmio.scuDma().lastTransfer()->complete);
    REQUIRE(!mmio.scuDma().descriptors()[0].requested);
}

void dispatchesScuDmaCompletionInterrupt() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address handlerAddress = 0x200U;
    constexpr core::Address source = 0x06005000U;
    constexpr core::Address destination = 0x06006000U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "dma-entry");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.setMemoryInterceptor(&mmio);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), 0U);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(platform::ScuEventScheduler::kScuInterruptTable + 0x4BU * 4U,
                    handlerAddress);
    state.write32BE(source, 0x12345678U);
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4, 0U));

    const auto dma = platform::ScuDmaRuntime::kDma0;
    REQUIRE(mmio.write(dma + 0x00U, 4, source));
    REQUIRE(mmio.write(dma + 0x04U, 4, destination));
    REQUIRE(mmio.write(dma + 0x08U, 4, 4U));
    REQUIRE(mmio.write(dma + 0x0CU, 4, 4U));
    REQUIRE(mmio.write(dma + 0x14U, 4, 7U));
    REQUIRE(mmio.write(dma + 0x10U, 4, 0x101U));
    REQUIRE(state.read32BE(destination) == 0x12345678U);

    platform::ScuEventScheduler scheduler;
    const auto delivered = scheduler.beforeBlock(entry, state, 0U, mmio);
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->target == handlerAddress);
    REQUIRE(delivered->returnMode == sir::InterruptReturnMode::Callback);
    REQUIRE((mmio.scuInterruptStatus() &
             platform::ScuEventScheduler::kLevel0DmaEndStatus) == 0U);
}

void dispatchesVdp1DrawEndInterrupt() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address handlerAddress = 0x200U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "vdp1-entry");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), 0U);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(platform::ScuEventScheduler::kScuInterruptTable +
                        platform::ScuEventScheduler::kSpriteDrawEndVector * 4U,
                    handlerAddress);
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4, 0U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp1Registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().frameCount() == 1U);
    REQUIRE(mmio.video().takeVdp1DrawEnd());

    platform::ScuEventScheduler scheduler;
    // Re-arm PTMR after observing that the explicit take consumed the first
    // edge, then let the scheduler translate it to SCU factor bit 13/vector 4D.
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp1Registers + 0x04U, 2, 1U));
    const auto delivered = scheduler.beforeBlock(entry, state, 0U, mmio);
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->target == handlerAddress);
    REQUIRE(delivered->returnMode == sir::InterruptReturnMode::Callback);
    REQUIRE((mmio.scuInterruptStatus() &
             platform::ScuEventScheduler::kSpriteDrawEndStatus) == 0U);
}

void dispatchesScuHblankBeforeFirstVdp1List() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address handlerAddress = 0x200U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "hblank-entry");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), 0U);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(platform::ScuEventScheduler::kScuInterruptTable +
                        platform::ScuEventScheduler::kHBlankInVector * 4U,
                    handlerAddress);
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4, 0U));

    // 526 blocks represent two blocks per scanline in the deterministic
    // scheduler. HBlank must therefore arrive on the second elapsed block,
    // without any PTMR write or VDP1 draw-end edge.
    platform::ScuEventScheduler scheduler(526U);
    REQUIRE(!scheduler.beforeBlock(entry, state, 0U, mmio).has_value());
    const auto delivered = scheduler.beforeBlock(entry, state, 1U, mmio);
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->target == handlerAddress);
    REQUIRE(delivered->returnMode == sir::InterruptReturnMode::Callback);
    REQUIRE(scheduler.stats().hblankEvents == 1U);
    REQUIRE(scheduler.stats().vblankEvents == 0U);
    REQUIRE(mmio.video().frameCount() == 0U);
}

void dispatchesScuVblankOutAfterVerticalInterval() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address handlerAddress = 0x200U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "vblank-out-entry");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), 0U);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(platform::ScuEventScheduler::kScuInterruptTable +
                        platform::ScuEventScheduler::kVBlankOutVector * 4U,
                    handlerAddress);
    // Mask HBlank while leaving both VBlank factors visible to this focused
    // phase test. VBlank-in has no target and is dropped at step zero.
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4,
                       platform::ScuEventScheduler::kHBlankInStatus));

    platform::ScuEventScheduler scheduler(526U); // two blocks per scanline
    scheduler.requestVBlank();
    REQUIRE(!scheduler.beforeBlock(entry, state, 0U, mmio).has_value());
    std::optional<sir::InterruptRequest> delivered;
    for (std::size_t step = 1U;
         step <= platform::ScuEventScheduler::kVBlankScanlines * 2U; ++step) {
        if (const auto event = scheduler.beforeBlock(entry, state, step, mmio)) {
            delivered = event;
        }
    }
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->target == handlerAddress);
    REQUIRE(delivered->returnMode == sir::InterruptReturnMode::Callback);
    REQUIRE(scheduler.stats().vblankEvents == 1U);
    REQUIRE(scheduler.stats().vblankOutEvents == 1U);
}

void readsIsoResourceFromCdImage() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(22U * platform::CdRuntime::kLogicalSectorSize);
    const auto putLe32 = [&image](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4U; ++index) {
            image[offset + index] = static_cast<std::byte>(value >> (index * 8U));
        }
    };
    const auto putRecord = [&image, &putLe32](std::size_t offset, std::uint32_t lba,
                                               std::uint32_t size, std::uint8_t flags,
                                               std::string_view name) {
        const auto length = static_cast<std::uint8_t>(33U + name.size() +
                                                       ((name.size() & 1U) == 0U ? 1U : 0U));
        image[offset] = static_cast<std::byte>(length);
        putLe32(offset + 2U, lba);
        putLe32(offset + 10U, size);
        image[offset + 25U] = static_cast<std::byte>(flags);
        image[offset + 32U] = static_cast<std::byte>(name.size());
        for (std::size_t index = 0; index < name.size(); ++index) {
            image[offset + 33U + index] = static_cast<std::byte>(name[index]);
        }
    };
    const auto pvd = 16U * platform::CdRuntime::kLogicalSectorSize;
    image[pvd] = std::byte{1};
    image[pvd + 1U] = std::byte{'C'};
    image[pvd + 2U] = std::byte{'D'};
    image[pvd + 3U] = std::byte{'0'};
    image[pvd + 4U] = std::byte{'0'};
    image[pvd + 5U] = std::byte{'1'};
    putRecord(pvd + 156U, 20U, platform::CdRuntime::kLogicalSectorSize, 2U,
              std::string_view{"\0", 1U});
    const auto directory = 20U * platform::CdRuntime::kLogicalSectorSize;
    putRecord(directory, 21U, 4U, 0U, "TEST.BIN;1");
    const auto data = 21U * platform::CdRuntime::kLogicalSectorSize;
    image[data] = std::byte{0xCA};
    image[data + 1U] = std::byte{0xFE};
    image[data + 2U] = std::byte{0xBA};
    image[data + 3U] = std::byte{0xBE};

    REQUIRE(cd.setImage(std::move(image)));
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEfls) != 0U);
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEfls)));
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEfls) == 0U);
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x18U, 2U, 0x7100U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x20U, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x24U, 2U, 2U));
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEfls) != 0U);
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x18U, 2U, 0x7200U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x20U, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 1U);
    REQUIRE(cd.commandRegister(2U) == 0x0100U);
    REQUIRE(cd.commandRegister(3U) == 2U);
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x18U, 2U, 0x7300U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x20U, 2U, 0x00FFU));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x24U, 2U, 0xFFFFU));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 6U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) != 0U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 171U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 4U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 0x00000200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) == 0U);
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x18U, 2U, 0x0600U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x20U, 2U, 0U));
    REQUIRE(cd.write(platform::CdRuntime::kCommandBase + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 6U);
    const auto entry = cd.findFile("/TEST.BIN");
    REQUIRE(entry.has_value());
    REQUIRE(entry->lba == 21U);
    REQUIRE(entry->size == 4U);
    const auto resource = cd.readFile("/TEST.BIN");
    REQUIRE(resource.has_value());
    REQUIRE(std::to_integer<std::uint8_t>((*resource)[0]) == 0xCAU);
    REQUIRE(std::to_integer<std::uint8_t>((*resource)[3]) == 0xBEU);
}

void completesCdSectorLengthAndPublishesSubcodeQ() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(platform::CdRuntime::kLogicalSectorSize);
    image[0] = std::byte{0x12};
    image[1] = std::byte{0x34};
    image[2] = std::byte{0x56};
    image[3] = std::byte{0x78};
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x6000U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0xFF00U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 0U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqScdq)));
    REQUIRE(cd.read(regs + 0x24U, 2U).has_value());
    cd.onSubcodeTick(1U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqScdq) != 0U);
    REQUIRE(cd.commandRegister(0U) == 0x2200U);
    REQUIRE(cd.commandRegister(1U) == 0x4101U);
    REQUIRE(cd.commandRegister(2U) == 0x0100U);
    REQUIRE(cd.commandRegister(3U) == 150U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x0200U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 204U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) != 0U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 0x41000096U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x0300U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 0U);
    REQUIRE(cd.commandRegister(2U) == 0x0100U);
    REQUIRE(cd.commandRegister(3U) == 151U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x48FCU));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0xFF00U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x4200U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x4401U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x4603U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0x00FFU));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x3000U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5000U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(1U) == 200U);
    REQUIRE(cd.commandRegister(2U) == 0x1800U);
    REQUIRE(cd.commandRegister(3U) == 200U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqDrdy)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5100U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(3U) == 0U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) != 0U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x1080U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 150U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0x0080U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 1U));
    REQUIRE(cd.commandRegister(0U) == 0x0300U);
    REQUIRE(cd.commandRegister(1U) == 0x4101U);
    REQUIRE(cd.commandRegister(2U) == 0x0100U);
    REQUIRE(cd.commandRegister(3U) == 150U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqCsct) == 0U);
    REQUIRE(cd.read(regs + 0x24U, 2U) == 150U);

    cd.onSubcodeTick(1U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqCsct) != 0U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqPend) != 0U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqScdq) != 0U);
    REQUIRE(cd.commandRegister(0U) == 0x2100U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5100U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(3U) == 1U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5200U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 1U));
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5300U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
    REQUIRE(cd.commandRegister(1U) == 1024U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x6100U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 1U));
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
    REQUIRE(cd.commandRegister(1U) == 0x4101U);
    REQUIRE(cd.commandRegister(2U) == 0x0100U);
    REQUIRE(cd.commandRegister(3U) == 151U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) != 0U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEhst) != 0U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 0x12345678U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x0600U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(1U) == 2U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x6200U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 1U));
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEhst) != 0U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqCsct) == 0U);

    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5100U));
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.commandRegister(3U) == 0U);
}

void completesCdSeekWithoutProducingStalePlaySectors() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(8U * platform::CdRuntime::kLogicalSectorSize);
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&cd, regs](std::uint16_t op, std::uint16_t cr2,
                                     std::uint16_t cr3, std::uint16_t cr4) {
        REQUIRE(cd.write(regs + 0x18U, 2U, op));
        REQUIRE(cd.write(regs + 0x1CU, 2U, cr2));
        REQUIRE(cd.write(regs + 0x20U, 2U, cr3));
        REQUIRE(cd.write(regs + 0x24U, 2U, cr4));
    };
    command(0x3000U, 0U, 0U, 0U);
    command(0x1080U, 150U, 0x0080U, 4U);
    cd.onSubcodeTick(1U);
    REQUIRE(cd.stats().subcodeProducedTicks == 1U);

    command(0x1180U, 155U, 0U, 0U);
    cd.onSubcodeTick(1U); // Complete the seek while its reply is still unread.
    REQUIRE(cd.read(regs + 0x18U, 2U) == 0x0400U);
    REQUIRE(cd.read(regs + 0x24U, 2U).has_value());
    cd.onSubcodeTick(1U);
    REQUIRE(cd.commandRegister(0U) == 0x2100U); // Periodic PAUSE, no endless SEEK.
    REQUIRE(cd.commandRegister(3U) == 155U);
    REQUIRE(cd.stats().subcodeProducedTicks == 1U);
    command(0x5100U, 0U, 0U, 0U);
    REQUIRE(cd.read(regs + 0x24U, 2U) == 1U); // Retained sector survives.

    command(0x1080U, 155U, 0x0080U, 3U);
    cd.onSubcodeTick(1U);
    REQUIRE(cd.stats().subcodeProducedTicks == 2U);
    command(0x11FFU, 0xFFFFU, 0U, 0U); // Pause must stop an active PLAY too.
    REQUIRE(cd.read(regs + 0x24U, 2U).has_value());
    cd.onSubcodeTick(1U);
    REQUIRE(cd.commandRegister(0U) == 0x2100U);
    REQUIRE(cd.commandRegister(3U) == 156U);
    REQUIRE(cd.stats().subcodeProducedTicks == 2U);
    command(0x1100U, 0U, 0U, 0U);
    REQUIRE(cd.read(regs + 0x24U, 2U).has_value());
    cd.onSubcodeTick(1U);
    REQUIRE(cd.commandRegister(0U) == 0x2200U);
    REQUIRE(cd.stats().subcodeProducedTicks == 2U);
}

void producesDoubleSpeedCdDataAtSubcodeCadence() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(160U * platform::CdRuntime::kLogicalSectorSize);
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&](std::uint16_t op, std::uint16_t cr2,
                             std::uint16_t cr3, std::uint16_t cr4) {
        REQUIRE(cd.write(regs + 0x18U, 2U, op));
        REQUIRE(cd.write(regs + 0x1CU, 2U, cr2));
        REQUIRE(cd.write(regs + 0x20U, 2U, cr3));
        REQUIRE(cd.write(regs + 0x24U, 2U, cr4));
    };
    command(0x3000U, 0U, 0U, 0U);
    command(0x1080U, 150U, 0x0080U, 151U);
    REQUIRE(cd.read(regs + 0x24U, 2U).has_value());
    for (int tick = 0; tick < 75; ++tick) cd.onSubcodeTick();
    REQUIRE(cd.stats().subcodeTicks == 75U);
    REQUIRE(cd.stats().lastStreamingCursor == 150U);
    REQUIRE(cd.commandRegister(3U) == 300U);
    cd.onSubcodeTick(); // A short final batch must stop exactly at the range end.
    REQUIRE(cd.stats().lastStreamingCursor == 151U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqPend) != 0U);
    cd.onSubcodeTick();
    REQUIRE(cd.stats().lastStreamingCursor == 151U);
}

void producesCdDataWhileHostDrainsFifo() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(4U * platform::CdRuntime::kLogicalSectorSize);
    std::fill_n(image.begin(), platform::CdRuntime::kLogicalSectorSize, std::byte{0x11});
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&](std::uint16_t op, std::uint16_t cr2,
                             std::uint16_t cr3, std::uint16_t cr4) {
        REQUIRE(cd.write(regs + 0x18U, 2U, op));
        REQUIRE(cd.write(regs + 0x1CU, 2U, cr2));
        REQUIRE(cd.write(regs + 0x20U, 2U, cr3));
        REQUIRE(cd.write(regs + 0x24U, 2U, cr4));
    };
    command(0x3000U, 0U, 0U, 0U);
    command(0x1080U, 150U, 0x0080U, 4U);
    cd.onSubcodeTick();
    command(0x6100U, 0U, 0U, 1U);
    cd.onSubcodeTick(); // Producer and host transfer advance independently.
    REQUIRE(cd.stats().lastStreamingCursor == 4U);
    REQUIRE(cd.stats().lastPartitionSectorCount == 4U);
    REQUIRE(cd.stats().subcodeBlockedTransferTicks == 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0300U); // Unread reply stays intact.
    REQUIRE(cd.read(regs + 0x24U, 2U) == 152U);
    for (unsigned word = 0; word < 512U; ++word) {
        REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 0x11111111U);
    }
    command(0x0600U, 0U, 0U, 0U);
    command(0x6200U, 0U, 0U, 1U);
    command(0x5100U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(3U) == 3U);
}

void preservesCdCommandPacketAcrossSubcodeTick() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(4U * platform::CdRuntime::kLogicalSectorSize);
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&cd, regs](std::uint16_t op, std::uint16_t cr2,
                                     std::uint16_t cr3, std::uint16_t cr4) {
        REQUIRE(cd.write(regs + 0x18U, 2U, op));
        REQUIRE(cd.write(regs + 0x1CU, 2U, cr2));
        REQUIRE(cd.write(regs + 0x20U, 2U, cr3));
        REQUIRE(cd.write(regs + 0x24U, 2U, cr4));
    };
    command(0x3000U, 0U, 0U, 0U);
    command(0x1080U, 150U, 0x0080U, 4U);
    REQUIRE(cd.read(regs + 0x24U, 2U) == 150U);
    cd.onSubcodeTick(1U);
    command(0x5100U, 0U, 0U, 0U);
    cd.onSubcodeTick(1U); // Must not replace the sector count with a periodic FAD.
    REQUIRE(cd.stats().lastStreamingCursor == 2U);
    REQUIRE(cd.read(regs + 0x18U, 2U) == 0x0300U);
    REQUIRE(cd.read(regs + 0x1CU, 2U) == 0U);
    REQUIRE(cd.read(regs + 0x20U, 2U) == 0U);
    REQUIRE(cd.read(regs + 0x26U, 2U) == 1U); // CR4 alias acknowledges the reply.
    cd.onSubcodeTick(1U);
    REQUIRE(cd.commandRegister(0U) == 0x2300U);
    REQUIRE(cd.commandRegister(3U) == 153U);

    // A tick between CR1 and CR4 must also preserve the command being written.
    REQUIRE(cd.write(regs + 0x18U, 2U, 0x5100U));
    cd.onSubcodeTick(1U);
    REQUIRE(cd.write(regs + 0x1CU, 2U, 0U));
    REQUIRE(cd.write(regs + 0x20U, 2U, 0U));
    REQUIRE(cd.write(regs + 0x24U, 2U, 0U));
    REQUIRE(cd.read(regs + 0x18U, 2U) == 0x0100U);
    REQUIRE(cd.read(regs + 0x24U, 2U) == 4U);
}

void waitsForCompleteCdCalculationRange() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(3U * platform::CdRuntime::kLogicalSectorSize);
    std::fill_n(image.begin(), platform::CdRuntime::kLogicalSectorSize, std::byte{0x11});
    std::fill_n(image.begin() + platform::CdRuntime::kLogicalSectorSize,
                platform::CdRuntime::kLogicalSectorSize, std::byte{0x22});
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&cd, regs](std::uint16_t op, std::uint16_t offset,
                                     std::uint16_t count) {
        REQUIRE(cd.write(regs + 0x18U, 2U, op));
        REQUIRE(cd.write(regs + 0x1CU, 2U, offset));
        REQUIRE(cd.write(regs + 0x20U, 2U, op == 0x1080U ? 0x0080U : 0U));
        REQUIRE(cd.write(regs + 0x24U, 2U, count));
        REQUIRE(cd.read(regs + 0x24U, 2U).has_value());
    };
    command(0x3000U, 0U, 0U);
    command(0x1080U, 150U, 3U);
    cd.onSubcodeTick(1U);
    command(0x5200U, 0U, 1U);
    command(0x5300U, 0U, 0U);
    REQUIRE(cd.commandRegister(1U) == 1024U);

    REQUIRE(cd.write(regs + 0x08U, 2U,
                     static_cast<std::uint16_t>(~platform::CdRuntime::kHirqEsel)));
    command(0x5200U, 0U, 2U);
    REQUIRE(cd.commandRegister(0U) == 0x8300U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) == 0U);
    command(0x5300U, 0U, 0U);
    REQUIRE(cd.commandRegister(1U) == 1024U); // WAIT preserves the last result.

    cd.onSubcodeTick(1U);
    command(0x5200U, 0U, 2U);
    REQUIRE(cd.commandRegister(0U) == 0x0300U);
    command(0x5300U, 0U, 0U);
    REQUIRE(cd.commandRegister(1U) == 2048U);
    command(0x6100U, 0U, 2U);
    for (unsigned word = 0; word < 1024U; ++word) {
        REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) ==
                (word < 512U ? 0x11111111U : 0x22222222U));
    }
    command(0x0600U, 0U, 0U);
    REQUIRE(cd.commandRegister(1U) == 2048U);
    command(0x6200U, 0U, 2U);
    command(0x5100U, 0U, 0U);
    REQUIRE(cd.commandRegister(3U) == 0U);
    cd.onSubcodeTick(1U);
    command(0x5200U, 0xFFFFU, 0xFFFFU);
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
    command(0x5300U, 0U, 0U);
    REQUIRE(cd.commandRegister(1U) == 1024U);
}

void queriesCdBlockHardwareSubcodeAndFilters() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(2U * platform::CdRuntime::kLogicalSectorSize);
    REQUIRE(cd.setImage(std::move(image)));
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEhst) != 0U);
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&cd, regs](std::uint16_t cr1, std::uint16_t cr2,
                                     std::uint16_t cr3, std::uint16_t cr4) {
        REQUIRE(cd.write(regs + 0x18U, 2U, cr1));
        REQUIRE(cd.write(regs + 0x1CU, 2U, cr2));
        REQUIRE(cd.write(regs + 0x20U, 2U, cr3));
        REQUIRE(cd.write(regs + 0x24U, 2U, cr4));
    };

    command(0x0100U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 0x0001U);
    REQUIRE(cd.commandRegister(2U) == 0U);
    REQUIRE(cd.commandRegister(3U) == 0x0400U);

    command(0x2000U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0200U);
    REQUIRE(cd.commandRegister(1U) == 5U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) != 0U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 0x41010100U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 4U) == 0x00000000U);
    REQUIRE(cd.read(platform::CdRuntime::kFifo, 2U) == 0x0200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqDrdy) == 0U);
    command(0x0600U, 0U, 0U, 0U);

    command(0x4012U, 0x3456U, 0x0301U, 0x2345U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqEsel) != 0U);
    command(0x4100U, 0U, 0x0300U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0212U);
    REQUIRE(cd.commandRegister(1U) == 0x3456U);
    REQUIRE(cd.commandRegister(2U) == 0x0001U);
    REQUIRE(cd.commandRegister(3U) == 0x2345U);

    command(0x425AU, 0xA5C3U, 0x037EU, 0x8134U);
    command(0x4300U, 0U, 0x0300U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x025AU);
    REQUIRE(cd.commandRegister(1U) == 0xA5C3U);
    REQUIRE(cd.commandRegister(2U) == 0x007EU);
    REQUIRE(cd.commandRegister(3U) == 0x8134U);

    // Mode 1 is a subheader predicate and is ignored for this MODE-1 data
    // image, leaving the filter's true connection observable below.
    command(0x4401U, 0U, 0x0300U, 0U);
    command(0x4500U, 0U, 0x0300U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0201U);
    command(0x4603U, 0x04FFU, 0x0300U, 0U);
    command(0x4700U, 0U, 0x0300U, 0U);
    REQUIRE(cd.commandRegister(1U) == 0x04FFU);

    // A flagless 0x48 resets only the requested partition.  Filter 3 must
    // survive so the next GFS PLAY keeps its selector graph.
    command(0x4800U, 0U, 0x0300U, 0U);
    command(0x4500U, 0U, 0x0300U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0201U);

    command(0x3000U, 0U, 0x0300U, 0U);
    command(0x3100U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(2U) == 0x0300U);

    command(0x1080U, 150U, 0x0080U, 1U);
    cd.onSubcodeTick(1U);
    command(0x3200U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(2U) == 0x0400U);
    // PLAY follows the selected filter's true connection (filter 3 ->
    // partition 4), not the filter number itself.
    command(0x5400U, 0U, 0x0400U, 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
    REQUIRE(cd.commandRegister(1U) == 150U);
}

void schedulesCdSubcodeIndependentlyFromVblank() {
    sir::Module module;
    auto& block = module.createBlock(0x100U, "cd-subcode");
    platform::MmioInterceptor mmio;
    std::vector<std::byte> image(platform::CdRuntime::kLogicalSectorSize);
    REQUIRE(mmio.cd().setImage(std::move(image)));
    REQUIRE(mmio.cd().write(platform::CdRuntime::kCommandBase + 0x08U, 2U,
                            static_cast<std::uint16_t>(
                                ~platform::CdRuntime::kHirqScdq)));
    // SCDQ is a playback/status factor, not a boot-time edge.  Start a
    // one-sector data play so the scheduler has an active subcode producer.
    REQUIRE(mmio.cd().write(platform::CdRuntime::kCommandBase + 0x18U, 2U,
                            0x1080U));
    REQUIRE(mmio.cd().write(platform::CdRuntime::kCommandBase + 0x1CU, 2U,
                            150U));
    REQUIRE(mmio.cd().write(platform::CdRuntime::kCommandBase + 0x20U, 2U,
                            0x0080U));
    REQUIRE(mmio.cd().write(platform::CdRuntime::kCommandBase + 0x24U, 2U,
                            1U));
    REQUIRE(mmio.cd().read(platform::CdRuntime::kCommandBase + 0x24U, 2U) == 150U);
    sir::MachineState state;
    platform::ScuEventScheduler scheduler(5U);
    for (std::size_t step = 0U; step < 3U; ++step) {
        REQUIRE(!scheduler.beforeBlock(block, state, step, mmio).has_value());
        REQUIRE((mmio.cd().hirq() & platform::CdRuntime::kHirqScdq) == 0U);
    }
    REQUIRE(!scheduler.beforeBlock(block, state, 3U, mmio).has_value());
    REQUIRE((mmio.cd().hirq() & platform::CdRuntime::kHirqScdq) != 0U);
    REQUIRE(scheduler.stats().cdSubcodeEvents == 1U);
    REQUIRE(scheduler.stats().vblankEvents == 0U);
}

void keepsCdCadenceWhenChangingCpuBudget() {
    sir::Module module;
    auto& block = module.createBlock(0x100U, "cd-cadence");
    for (const std::size_t budget : {10'000U, 14'000U, 20'000U}) {
        platform::MmioInterceptor mmio;
        mmio.setAccessRecording(false);
        REQUIRE(mmio.cd().setImage(std::vector<std::byte>(
            160U * platform::CdRuntime::kLogicalSectorSize)));
        const auto regs = platform::CdRuntime::kCommandBase;
        const auto command = [&](std::uint16_t op, std::uint16_t cr2,
                                 std::uint16_t cr3, std::uint16_t cr4) {
            REQUIRE(mmio.cd().write(regs + 0x18U, 2U, op));
            REQUIRE(mmio.cd().write(regs + 0x1CU, 2U, cr2));
            REQUIRE(mmio.cd().write(regs + 0x20U, 2U, cr3));
            REQUIRE(mmio.cd().write(regs + 0x24U, 2U, cr4));
        };
        command(0x3000U, 0U, 0U, 0U);
        command(0x1080U, 150U, 0x0080U, 160U);
        platform::ScuEventScheduler scheduler(budget);
        sir::MachineState state;
        for (std::size_t step = 0U; step < budget * 60U; ++step) {
            static_cast<void>(scheduler.beforeBlock(block, state, step, mmio));
        }
        REQUIRE(scheduler.stats().vblankEvents == 60U);
        REQUIRE(scheduler.stats().cdSubcodeEvents == 75U);
        REQUIRE(mmio.cd().stats().subcodeProducedTicks == 150U);
    }
}

void streamsCdPlayPastRetainedBufferCapacity() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(
        300U * platform::CdRuntime::kLogicalSectorSize);
    REQUIRE(cd.setImage(std::move(image)));
    const auto regs = platform::CdRuntime::kCommandBase;
    const auto command = [&cd, regs](std::uint16_t cr1, std::uint16_t cr2,
                                     std::uint16_t cr3, std::uint16_t cr4) {
        REQUIRE(cd.write(regs + 0x18U, 2U, cr1));
        REQUIRE(cd.write(regs + 0x1CU, 2U, cr2));
        REQUIRE(cd.write(regs + 0x20U, 2U, cr3));
        REQUIRE(cd.write(regs + 0x24U, 2U, cr4));
    };

    command(0x3000U, 0U, 0U, 0U);
    command(0x1080U, 150U, 0x0080U, 250U);
    REQUIRE(cd.stats().lastPlayAvailable == 300U);
    REQUIRE(cd.stats().lastStreamingSectorCount == 250U);
    for (std::size_t sector = 0U; sector < 200U; ++sector) {
        cd.onSubcodeTick(1U);
    }
    command(0x5100U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(3U) == 200U);

    // A full retained partition applies backpressure to PLAY instead of
    // truncating the requested range or dropping sectors.
    for (std::size_t tick = 0U; tick < 10U; ++tick) cd.onSubcodeTick(1U);
    REQUIRE(cd.stats().lastStreamingCursor == 200U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqPend) == 0U);

    command(0x6200U, 0U, 0U, 50U);
    command(0x5100U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(3U) == 150U);
    for (std::size_t sector = 0U; sector < 50U; ++sector) {
        cd.onSubcodeTick(1U);
    }
    command(0x5100U, 0U, 0U, 0U);
    REQUIRE(cd.commandRegister(3U) == 200U);
    REQUIRE(cd.stats().lastStreamingCursor == 250U);
    REQUIRE((cd.hirq() & platform::CdRuntime::kHirqPend) != 0U);
    REQUIRE(cd.commandRegister(0U) == 0x0100U);
}

void dmaLoadsIsoResourceIntoVdp2() {
    platform::CdRuntime cd;
    std::vector<std::byte> image(22U * platform::CdRuntime::kLogicalSectorSize);
    const auto putLe32 = [&image](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4U; ++index) {
            image[offset + index] = static_cast<std::byte>(value >> (index * 8U));
        }
    };
    const auto putRecord = [&image, &putLe32](std::size_t offset, std::uint32_t lba,
                                               std::uint32_t size, std::uint8_t flags,
                                               std::string_view name) {
        const auto length = static_cast<std::uint8_t>(33U + name.size() +
                                                       ((name.size() & 1U) == 0U ? 1U : 0U));
        image[offset] = static_cast<std::byte>(length);
        putLe32(offset + 2U, lba);
        putLe32(offset + 10U, size);
        image[offset + 25U] = static_cast<std::byte>(flags);
        image[offset + 32U] = static_cast<std::byte>(name.size());
        for (std::size_t index = 0; index < name.size(); ++index) {
            image[offset + 33U + index] = static_cast<std::byte>(name[index]);
        }
    };
    const auto pvd = 16U * platform::CdRuntime::kLogicalSectorSize;
    image[pvd] = std::byte{1};
    image[pvd + 1U] = std::byte{'C'};
    image[pvd + 2U] = std::byte{'D'};
    image[pvd + 3U] = std::byte{'0'};
    image[pvd + 4U] = std::byte{'0'};
    image[pvd + 5U] = std::byte{'1'};
    putRecord(pvd + 156U, 20U, platform::CdRuntime::kLogicalSectorSize, 2U,
              std::string_view{"\0", 1U});
    const auto directory = 20U * platform::CdRuntime::kLogicalSectorSize;
    putRecord(directory, 21U, 4U, 0U, "TEST.BIN;1");
    const auto data = 21U * platform::CdRuntime::kLogicalSectorSize;
    image[data] = std::byte{0xCA};
    image[data + 1U] = std::byte{0xFE};
    image[data + 2U] = std::byte{0xBA};
    image[data + 3U] = std::byte{0xBE};

    REQUIRE(cd.setImage(std::move(image)));
    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.setMemoryInterceptor(&mmio);
    const auto transfer = cd.dmaReadFile("/TEST.BIN", platform::VideoRuntime::kVdp2Base,
                                         state);
    REQUIRE(transfer.has_value());
    REQUIRE(transfer->complete);
    REQUIRE(transfer->requested == 4U);
    REQUIRE(transfer->transferred == 4U);
    REQUIRE(transfer->sectors == 1U);
    REQUIRE(mmio.video().read(platform::VideoRuntime::kVdp2Base, 4) == 0xCAFEBABEU);
}

void commitsVdp1FramebufferAsFrame() {
    platform::MmioInterceptor mmio;
    const auto framebuffer = platform::VideoRuntime::kVdp1Framebuffer;
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    REQUIRE(mmio.write(framebuffer, 2, 0x801FU));
    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().frameCount() == 1U);
    REQUIRE(mmio.video().framePending());
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.sequence == 1U);
    REQUIRE(frame.width == platform::VideoRuntime::kFrameWidth);
    REQUIRE(frame.height == platform::VideoRuntime::kFrameHeight);
    REQUIRE(frame.pixels[0] == 0xFFFF0000U);
    REQUIRE(!mmio.video().framePending());

    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers, 2, 0x8000U));
    REQUIRE(mmio.video().displayEnabled());
    REQUIRE(mmio.read(platform::VideoRuntime::kVdp2Registers + 0x04U, 2) == 0U);
}

void composesVdp2LayersBehindVdp1() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto vdp2Registers = platform::VideoRuntime::kVdp2Registers;
    const auto vdp1Framebuffer = platform::VideoRuntime::kVdp1Framebuffer;

    // TVMD display, NBG0 enabled in 32K-color bitmap mode (512x256); BACK
    // is the implicit fallback surface underneath enabled scroll screens.
    REQUIRE(mmio.write(vdp2Registers + 0x000U, 2, 0x8000U));
    REQUIRE(mmio.write(vdp2Registers + 0x020U, 2, 0x0001U));
    REQUIRE(mmio.write(vdp2Registers + 0x028U, 2, 0x0032U));
    REQUIRE(mmio.write(vdp2Registers + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(vdp2Registers + 0x03CU, 2, 0U));
    REQUIRE(mmio.write(vdp2Registers + 0x0ACU, 2, 0U));
    REQUIRE(mmio.write(vdp2Registers + 0x0AEU, 2, 0x0100U));
    REQUIRE(mmio.write(vdp2 + 0x0200U, 2, 0x7C00U)); // blue BACK
    REQUIRE(mmio.write(vdp2 + 0x0000U, 2, 0x001FU)); // red NBG0 pixel 0
    REQUIRE(mmio.write(vdp2 + 0x0002U, 2, 0x001FU)); // red NBG0 pixel 1
    REQUIRE(mmio.write(vdp2Registers + 0x0E0U, 2, 0x0025U)); // mixed RGB sprite data
    REQUIRE(mmio.write(vdp2Registers + 0x0F0U, 2, 0x0001U)); // RGB uses sprite register 0
    REQUIRE(mmio.write(vdp1Framebuffer, 2, 0x83E0U)); // green VDP1 overlay
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp1Registers + 0x04U, 2, 1U));

    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFF00FF00U);
    REQUIRE(frame.pixels[1] == 0xFFFF0000U);
    REQUIRE(frame.pixels[2] == 0xFF0000FFU);
}

void comparesVdp1SpritePriorityPerPixelWithVdp2Layer() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto vdp2Registers = platform::VideoRuntime::kVdp2Registers;
    const auto vdp1 = platform::VideoRuntime::kVdp1Base;
    const auto vdp1Registers = platform::VideoRuntime::kVdp1Registers;
    const auto writeVdp1 = [&mmio, vdp1](std::uint32_t offset, std::uint16_t value) {
        REQUIRE(mmio.write(vdp1 + offset, 2, value));
    };

    REQUIRE(mmio.write(vdp2Registers + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(vdp2Registers + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(vdp2Registers + 0x028U, 2, 0x0032U)); // RGB555 bitmap
    REQUIRE(mmio.write(vdp2Registers + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(vdp2Registers + 0x03CU, 2, 0U));
    REQUIRE(mmio.write(vdp2Registers + 0x0AEU, 2, 0x0100U)); // BACK elsewhere
    REQUIRE(mmio.write(vdp2Registers + 0x0E0U, 2, 0x0025U)); // mixed, SPTYPE 5
    REQUIRE(mmio.write(vdp2Registers + 0x00EU, 2, 0U)); // 1024-color CRAM mode
    REQUIRE(mmio.write(vdp2Registers + 0x0E6U, 2, 0x0010U)); // SPCAOS = 1
    REQUIRE(mmio.write(vdp2Registers + 0x0F2U, 2, 0x0400U)); // sprite register 3 = 4
    REQUIRE(mmio.write(vdp2Registers + 0x0F8U, 2, 0x0005U)); // NBG0 priority = 5
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Cram + 0x0402U, 2, 0x03E0U));
    REQUIRE(mmio.write(vdp2 + 0x0000U, 2, 0x001FU)); // red NBG0 pixel 0
    REQUIRE(mmio.write(vdp2 + 0x0002U, 2, 0x7C00U)); // blue NBG0 pixel 1

    // A 4-bit texture with one palette texel and one transparent texel.
    // CMDCOLR 0x3000 carries SPTYPE-5 priority selector 3.
    writeVdp1(0x000U, 0x0000U); // normal sprite
    writeVdp1(0x004U, 0x0000U); // 4-bit bank mode
    writeVdp1(0x006U, 0x3000U);
    writeVdp1(0x008U, 0x0040U); // source = VRAM+0x200
    writeVdp1(0x00AU, 0x0101U); // 8x1
    writeVdp1(0x00CU, 0U); writeVdp1(0x00EU, 0U);
    writeVdp1(0x200U, 0x1000U); // pixel 0 = CRAM index 1; pixel 1 = transparent
    writeVdp1(0x020U, 0x8000U); // end marker

    // NBG0 at priority 5 covers the sprite at priority 4. The transparent
    // framebuffer texel at x=1 must reveal the blue NBG0 pixel unchanged.
    REQUIRE(mmio.write(vdp2Registers + 0x0F8U, 2, 0x0005U));
    REQUIRE(mmio.write(vdp1Registers + 0x04U, 2, 1U));
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);
    REQUIRE(frame.pixels[1U] == 0xFF0000FFU);
    REQUIRE(mmio.read(platform::VideoRuntime::kVdp1Framebuffer, 2) == 0x3001U);
    REQUIRE(mmio.read(platform::VideoRuntime::kVdp1Framebuffer + 2U, 2) == 0U);

    // Lowering NBG0 to priority 3 lets the same sprite win at x=0.
    REQUIRE(mmio.write(vdp2Registers + 0x0F8U, 2, 0x0003U));
    REQUIRE(mmio.write(vdp1Registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF00FF00U);
    REQUIRE(frame.pixels[1U] == 0xFF0000FFU);

    // Equal priorities are resolved in hardware order: sprite is above NBG0.
    REQUIRE(mmio.write(vdp2Registers + 0x0F8U, 2, 0x0004U));
    REQUIRE(mmio.write(vdp1Registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF00FF00U);
    REQUIRE(frame.pixels[1U] == 0xFF0000FFU);

    // A programmed sprite priority of zero is transparent, even over a lower
    // priority NBG0 pixel.
    REQUIRE(mmio.write(vdp2Registers + 0x0F2U, 2, 0x0004U)); // S3PRIN=0
    REQUIRE(mmio.write(vdp2Registers + 0x0F8U, 2, 0x0003U));
    REQUIRE(mmio.write(vdp1Registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);
    REQUIRE(frame.pixels[1U] == 0xFF0000FFU);
}

void calculatesVdp2ColorBetweenTopAndSecondLayers() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;

    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0003U)); // NBG0 and NBG1
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x3232U)); // both RGB555 bitmaps
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0x0010U)); // NBG1 bitmap base 0x20000
    REQUIRE(mmio.write(regs + 0x0AEU, 2, 0x7FFFU)); // BACK away from the layers
    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0305U)); // NBG0=5, NBG1=3
    REQUIRE(mmio.write(regs + 0x108U, 2, 0x0010U)); // NBG0 ratio=16
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0x0001U)); // NBG0 color calculation
    REQUIRE(mmio.write(vdp2, 2, 0x03E0U));          // NBG0 green
    REQUIRE(mmio.write(vdp2 + 0x20000U, 2, 0x001FU)); // NBG1 red

    platform::VideoFrame frame;
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    // ratio 16: (top*15 + second*17) / 32, independently per channel.
    REQUIRE(frame.pixels[0U] == 0xFF877700U);

    // CCRTMD selects the ratio from the immediately lower image instead of
    // NBG0. NBG1 ratio 0 leaves one part of the lower image in the result.
    REQUIRE(mmio.write(regs + 0x108U, 2, 0x0010U));
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0x0201U)); // CCRTMD + NBG0 CC
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF07F700U);

    // NBG1 ratio 31 selects the lower image completely.
    REQUIRE(mmio.write(regs + 0x108U, 2, 0x1F10U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);

    // CCMD adds the two inputs directly and saturates each color channel.
    REQUIRE(mmio.write(regs + 0x108U, 2, 0x0010U));
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0x0101U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFFFF00U);

    // Disabling NBG0 color calculation restores its unblended color.
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF00FF00U);
}

void calculatesVdp1SpriteColorAndVdp2Shadows() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    const auto framebuffer = platform::VideoRuntime::kVdp1Framebuffer;

    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0032U)); // RGB555 bitmap
    REQUIRE(mmio.write(regs + 0x0AEU, 2, 0x0100U)); // BACK at 0x200
    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0003U)); // NBG0 priority=3
    REQUIRE(mmio.write(regs + 0x0E0U, 2, 0x0725U)); // mixed, SPTYPE5, condition <= 7
    REQUIRE(mmio.write(regs + 0x0F2U, 2, 0x0400U)); // sprite register 3 priority=4
    REQUIRE(mmio.write(regs + 0x100U, 2, 0x1000U)); // sprite ratio 1 = 16
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0x0040U)); // sprite color calculation
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Cram + 2U, 2, 0x03E0U));
    REQUIRE(mmio.write(vdp2, 2, 0x001FU)); // red NBG0

    platform::VideoFrame frame;
    REQUIRE(mmio.write(framebuffer, 2, 0x3801U)); // S3 priority, CC index 1, CRAM 1
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF877700U);

    // The global SPCCEN gate makes the same sprite opaque when cleared.
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF00FF00U);

    // SPCCCS can reject color calculation even with SPCCEN set.
    REQUIRE(mmio.write(regs + 0x0ECU, 2, 0x0040U));
    REQUIRE(mmio.write(regs + 0x0E0U, 2, 0x2705U)); // priority >= 7 (false here)
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF00FF00U);

    // A normal shadow uses the type-5 0x7FE color code and SDCTL's NBG0 bit.
    REQUIRE(mmio.write(regs + 0x0E0U, 2, 0x0705U)); // palette-only type 5
    REQUIRE(mmio.write(regs + 0x0E2U, 2, 0x0001U)); // NBG0 shadow enabled
    REQUIRE(mmio.write(framebuffer, 2, 0x37FEU));   // S3 priority, normal shadow
    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0005U)); // background now outranks sprite
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);

    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0003U)); // sprite wins again
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF7F0000U);

    // MSB shadow also darkens the winning NBG0 pixel in palette-only mode.
    REQUIRE(mmio.write(regs + 0x0F0U, 2, 0x0004U)); // sprite register 0 priority=4
    REQUIRE(mmio.write(framebuffer, 2, 0x8001U));   // sprite shadow
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF7F0000U);

    // Transparent MSB shadows require TPSDSL and shade only the immediately
    // lower-priority screen.
    REQUIRE(mmio.write(regs + 0x0E2U, 2, 0x0001U)); // TPSDSL disabled
    REQUIRE(mmio.write(framebuffer, 2, 0x8000U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);

    REQUIRE(mmio.write(regs + 0x0E2U, 2, 0x0101U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF7F0000U);
}

void publishesVdp2BackAndHonorsExplicitPriority() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;

    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x0ACU, 2, 0x8000U)); // per-line BACK table
    REQUIRE(mmio.write(regs + 0x0AEU, 2, 0x0200U));
    REQUIRE(mmio.write(vdp2 + 0x0400U, 2, 0x001FU)); // red line 0
    REQUIRE(mmio.write(vdp2 + 0x0402U, 2, 0x03E0U)); // green line 1
    mmio.video().advanceFrame();

    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);
    REQUIRE(frame.pixels[platform::VideoRuntime::kFrameWidth] == 0xFF00FF00U);

    // A written priority register makes priority zero an explicit disable;
    // it must not overwrite the published BACK surface.
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U));
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0042U));
    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0000U));
    REQUIRE(mmio.write(vdp2, 4, 0x80402010U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);
}

void resolvesVdp2LayerTiesInHardwareOrder() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;

    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0003U)); // NBG0 and NBG1
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x3232U)); // both 32K-color bitmaps
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0x0010U)); // NBG1 bitmap base 0x20000
    REQUIRE(mmio.write(regs + 0x0AEU, 2, 0x7FFFU)); // BACK away from both layers
    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0404U)); // both priorities = 4
    REQUIRE(mmio.write(vdp2, 2, 0x001FU));          // NBG0 red
    REQUIRE(mmio.write(vdp2 + 0x20000U, 2, 0x03E0U)); // NBG1 green

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U); // NBG0 wins the equal-priority tie
}

void capturesVdp2PriorityStateWithDiagnosticMemory() {
    platform::VideoRuntime video;
    platform::VideoDiagnosticSnapshot snapshot;
    bool captured{};
    video.setDiagnosticSink([&](const auto& value) {
        snapshot = value;
        captured = true;
    });
    video.setDiagnosticMemoryCapture(true);

    REQUIRE(video.write(platform::VideoRuntime::kVdp2Registers + 0x0F8U,
                        2U, 0x0206U));
    video.advanceFrame();
    video.presentFrame();

    REQUIRE(captured);
    REQUIRE(snapshot.vdp2Prina == 0x0206U);
    REQUIRE(snapshot.priorityConfigured);
}

void composesVdp2Rgb888BitmapWithoutAlphaBit() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0042U)); // 24-bit RGB bitmap, 512x256
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));
    // VRAM order is an unused byte followed by B, G, R. The high bit of B
    // must not be interpreted as an alpha bit by the decoder.
    REQUIRE(mmio.write(vdp2, 4, 0x80402010U));

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFF102040U);
}

void composesVdp2Continuous320BitmapWithLineAndCellScroll() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0042U)); // 32-bit RGB, 512x256 aperture
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));
    // N0 line-scroll horizontal/vertical + vertical-cell scroll enabled,
    // with one table entry per non-interlaced line.
    REQUIRE(mmio.write(regs + 0x09AU, 2, 0x0007U));
    REQUIRE(mmio.write(regs + 0x0A0U, 2, 0x0003U)); // LSTA0 = 0x60000
    REQUIRE(mmio.write(regs + 0x0A2U, 2, 0x0000U));
    REQUIRE(mmio.write(regs + 0x09CU, 2, 0x0003U)); // VCSTA = 0x70000
    REQUIRE(mmio.write(regs + 0x09EU, 2, 0x8000U));
    // Line 1 starts at bitmap x=320 and has a 0.625-pixel vertical
    // component.  Cell 24 adds 0x6000, so x=192 crosses the 512-wide
    // aperture into the next physical row.
    REQUIRE(mmio.write(vdp2 + 0x60008U, 4, 0x01400000U));
    REQUIRE(mmio.write(vdp2 + 0x6000CU, 4, 0x0000A000U));
    REQUIRE(mmio.write(vdp2 + 0x70060U, 4, 0x00006000U));
    // The packed image's row 1 starts at +0x500.  The physical 512x256
    // aperture sees it first at row 0,x=320 and then at row 1,x=0.
    REQUIRE(mmio.write(vdp2 + 0x0500U, 4, 0x80402010U));
    REQUIRE(mmio.write(vdp2 + 0x0800U, 4, 0x8000FF00U));

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[platform::VideoRuntime::kFrameWidth] == 0xFF102040U);
    REQUIRE(frame.pixels[platform::VideoRuntime::kFrameWidth + 192U] ==
            0xFF00FF00U);
}

void excludesVdp2LayersWhoseFetchSlotsAreUsedByHighColorModes() {
    platform::MmioInterceptor mmio;
    const auto vram = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs, 2, 0x8000U));
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x000FU));
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x3242U)); // N0 RGB888, N1 RGB555
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0x0010U)); // N1 bitmap at 0x20000
    REQUIRE(mmio.write(regs + 0x048U, 2, 0x0020U)); // N2 map at 0x40000
    REQUIRE(mmio.write(regs + 0x04CU, 2, 0x0020U)); // N3 map at 0x40000
    REQUIRE(mmio.write(regs + 0x034U, 2, 0x8000U));
    REQUIRE(mmio.write(regs + 0x036U, 2, 0x8000U));
    REQUIRE(mmio.write(regs + 0x0F8U, 2, 0x0501U));
    REQUIRE(mmio.write(regs + 0x0FAU, 2, 0x0706U));
    REQUIRE(mmio.write(vram, 4, 0x8000FF00U)); // N0 green, next dot transparent
    REQUIRE(mmio.write(vram + 0x20000U, 2, 0x801FU)); // N1 red
    REQUIRE(mmio.write(vram + 0x40000U, 2, 4U)); // N2/N3 tile 1
    REQUIRE(mmio.write(vram + 0x20U, 4, 0x11111111U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Cram + 2U, 2, 0x7C00U));
    platform::VideoDiagnosticSnapshot snapshot;
    mmio.video().setDiagnosticSink([&](const auto& value) { snapshot = value; });
    const auto capture = [&] {
        mmio.video().advanceFrame();
        mmio.video().presentFrame();
    };
    const auto blank = [&](unsigned layer) {
        return std::all_of(snapshot.vdp2Layers[layer].pixels.begin(),
                           snapshot.vdp2Layers[layer].pixels.end(),
                           [](auto pixel) { return pixel == 0U; });
    };
    capture();
    REQUIRE(snapshot.composed.pixels[0] == 0xFF00FF00U);
    REQUIRE(snapshot.composed.pixels[1] == 0xFF000000U);
    REQUIRE(blank(1) && blank(2) && blank(3));

    // RGB555 N0 takes N2's slots. RGB555 N1 takes N3's slots.
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x3232U));
    capture();
    REQUIRE(!blank(1));
    REQUIRE(blank(2) && blank(3));

    // Leaving the conflicting modes must restore the other layers.
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x000CU));
    capture();
    REQUIRE(!blank(2) && !blank(3));

    mmio.video().setDiagnosticCaptureEnabled(false);
    const auto previous = snapshot.composed.sequence;
    capture();
    REQUIRE(snapshot.composed.sequence == previous);
    mmio.video().setDiagnosticCaptureEnabled(true);
    capture();
    REQUIRE(snapshot.composed.sequence == previous + 2U);
}

void composesVdp2ContinuousBitmapAddsRelativeScrollBase() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0042U)); // 32-bit RGB bitmap
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x074U, 2, 1U)); // SCYIN0 base row
    REQUIRE(mmio.write(regs + 0x09AU, 2, 0x0007U)); // line/cell scroll
    REQUIRE(mmio.write(regs + 0x0A0U, 2, 0x0003U)); // LSTA0 = 0x60000
    REQUIRE(mmio.write(regs + 0x0A2U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x09CU, 2, 0x0003U)); // VCSTA = 0x70000
    REQUIRE(mmio.write(regs + 0x09EU, 2, 0x8000U));
    // The table entry is relative. With SCYIN0=1, the first displayed pixel
    // must come from physical bitmap row 1 rather than row 0.
    REQUIRE(mmio.write(vdp2 + 0x0800U, 4, 0x8000FF00U));

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFF00FF00U);
}

void composesVdp2ContinuousBitmapWithInterleavedCellScroll() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0042U)); // 32-bit RGB bitmap
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));
    // N0 line-scroll + vertical-cell scroll.  This resource stores the NBG0
    // cell values in the second longword of each 8-byte interleaved slot.
    REQUIRE(mmio.write(regs + 0x09AU, 2, 0x0007U));
    REQUIRE(mmio.write(regs + 0x0A0U, 2, 0x0003U)); // LSTA0 = 0x60000
    REQUIRE(mmio.write(regs + 0x0A2U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x09CU, 2, 0x0003U)); // VCSTA = 0x70000
    REQUIRE(mmio.write(regs + 0x09EU, 2, 0x8000U));
    REQUIRE(mmio.write(vdp2 + 0x60008U, 4, 0x01400000U));
    REQUIRE(mmio.write(vdp2 + 0x6000CU, 4, 0x0000A000U));
    // Four non-zero interleaved entries make the layout unambiguous to the
    // same stride detector used by the runtime; cell 24 crosses row 0->1.
    REQUIRE(mmio.write(vdp2 + 0x7000CU, 4, 0x00000400U));
    REQUIRE(mmio.write(vdp2 + 0x70014U, 4, 0x00000800U));
    REQUIRE(mmio.write(vdp2 + 0x7001CU, 4, 0x00000C00U));
    REQUIRE(mmio.write(vdp2 + 0x700C4U, 4, 0x00006000U));
    REQUIRE(mmio.write(vdp2 + 0x0500U, 4, 0x80402010U));
    REQUIRE(mmio.write(vdp2 + 0x0800U, 4, 0x8000FF00U));

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[platform::VideoRuntime::kFrameWidth] == 0xFF102040U);
    REQUIRE(frame.pixels[platform::VideoRuntime::kFrameWidth + 192U] ==
            0xFF00FF00U);
}

void composesVdp2CellPatternNameAndTile() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;

    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0030U)); // NBG0, 32768-color cell
    REQUIRE(mmio.write(regs + 0x030U, 2, 0x8000U)); // one-word PNT, mode 0
    REQUIRE(mmio.write(regs + 0x03AU, 2, 0U));      // 1x1 page
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));      // map offset
    REQUIRE(mmio.write(regs + 0x040U, 2, 0U));      // plane A page 0
    REQUIRE(mmio.write(regs + 0x042U, 2, 0U));
    REQUIRE(mmio.write(vdp2 + 0x0000U, 2, 0x0004U)); // character #1
    for (std::size_t row = 0; row < 8U; ++row) {
        for (std::size_t column = 0; column < 8U; ++column) {
            const auto pixelOffset = static_cast<core::Address>((row * 8U + column) * 2U);
            REQUIRE(mmio.write(vdp2 + 0x0080U + pixelOffset,
                               2, 0x801FU)); // opaque RGB555 red
        }
    }

    platform::VideoFrame frame;
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFFFF0000U);
    REQUIRE(frame.pixels[7] == 0xFFFF0000U);
}

void addressesVdp2PatternNamePlanes() {
    // Independent table 4.8 expectations for combined map value 0x73,
    // in 4-Mbit VRAM: [character size][name size][plane size].
    constexpr core::Address bases[2][2][3] = {
        {{0x66000U, 0x64000U, 0x60000U}, {0x4C000U, 0x48000U, 0x40000U}},
        {{0x39800U, 0x39000U, 0x38000U}, {0x73000U, 0x72000U, 0x70000U}},
    };
    for (unsigned layer = 0; layer < 2; ++layer) {
        for (unsigned large = 0; large < 2; ++large) {
            for (unsigned twoWord = 0; twoWord < 2; ++twoWord) {
                for (unsigned size = 0; size < 3; ++size) {
                    platform::MmioInterceptor mmio;
                    const auto regs = platform::VideoRuntime::kVdp2Registers;
                    const auto vram = platform::VideoRuntime::kVdp2Base;
                    REQUIRE(mmio.write(regs, 2, 0x8000U));
                    REQUIRE(mmio.write(regs + 0x020U, 2, 1U << layer));
                    REQUIRE(mmio.write(regs + 0x028U, 2, large << (layer * 8U)));
                    REQUIRE(mmio.write(regs + 0x030U + layer * 2U, 2,
                                       twoWord ? 0U : 0x8000U));
                    REQUIRE(mmio.write(regs + 0x03AU, 2,
                                       (size == 2U ? 3U : size) << (layer * 2U)));
                    REQUIRE(mmio.write(regs + 0x03CU, 2, 1U << (layer * 4U)));
                    REQUIRE(mmio.write(regs + 0x040U + layer * 4U, 2, 0x33U));
                    // Character zero, palette one. A wrong base reads palette zero.
                    REQUIRE(mmio.write(vram + bases[large][twoWord][size], 2,
                                       twoWord ? 1U : 0x1000U));
                    REQUIRE(mmio.write(vram, 2, 0x1111U));
                    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Cram + 34U,
                                       2, 0x001FU));
                    platform::VideoFrame frame;
                    mmio.video().advanceFrame();
                    REQUIRE(mmio.video().consumeFrame(frame));
                    REQUIRE(frame.pixels[0] == 0xFFFF0000U);
                }
            }
        }
    }
}

void composesVdp2PagesWithinMultiPagePlanes() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;

    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0030U)); // 32K-color, 8x8 cells
    REQUIRE(mmio.write(regs + 0x030U, 2, 0U));      // two-word PNT
    REQUIRE(mmio.write(regs + 0x03AU, 2, 0x0001U)); // 2 horizontal pages
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));      // map offset
    REQUIRE(mmio.write(regs + 0x040U, 2, 0U));      // plane A starts at 0
    REQUIRE(mmio.write(regs + 0x042U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x070U, 2, 0x0200U)); // enter page 1 inside plane A

    // A 2x1 plane stores its 8x8/two-word pages consecutively: page 1 starts
    // 0x4000 bytes after plane A. Pattern names address characters in 32-byte
    // units, so the RGB555 tile at 0x80 uses character number 4.
    REQUIRE(mmio.write(vdp2 + 0x4000U, 2, 0U));
    REQUIRE(mmio.write(vdp2 + 0x4002U, 2, 4U));
    for (std::size_t pixel = 0; pixel < 64U; ++pixel) {
        REQUIRE(mmio.write(vdp2 + 0x0080U +
                               static_cast<core::Address>(pixel * 2U),
                           2, 0x801FU)); // opaque RGB555 red
    }

    platform::VideoFrame frame;
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFFFF0000U);

    // Plane B starts after the 2-page-wide plane A. Give B a distinct aligned
    // map number and prove that crossing the plane boundary selects it.
    REQUIRE(mmio.write(regs + 0x040U, 2, 0x0200U)); // plane B map number 2
    REQUIRE(mmio.write(regs + 0x070U, 2, 0x0400U)); // plane B starts at x=1024
    REQUIRE(mmio.write(vdp2 + 0x8000U, 2, 0U));
    REQUIRE(mmio.write(vdp2 + 0x8002U, 2, 4U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFFFF0000U);

    // In a 2x2-page plane, pages are laid out row-major and the lower-right
    // page is the fourth consecutive name table.
    REQUIRE(mmio.write(regs + 0x03AU, 2, 0x0003U)); // 2 horizontal x 2 vertical
    REQUIRE(mmio.write(regs + 0x040U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x042U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x070U, 2, 0x0200U));
    REQUIRE(mmio.write(regs + 0x074U, 2, 0x0200U));
    REQUIRE(mmio.write(vdp2 + 0xC000U, 2, 0U));
    REQUIRE(mmio.write(vdp2 + 0xC002U, 2, 4U));
    mmio.video().advanceFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFFFF0000U);
}

void appliesVdp2RectangularWindowToNbg0() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0032U)); // 32K bitmap
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0ACU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0AEU, 2, 0x0100U));
    REQUIRE(mmio.write(regs + 0x0C0U, 2, 1U));
    REQUIRE(mmio.write(regs + 0x0C2U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0C4U, 2, 2U));
    REQUIRE(mmio.write(regs + 0x0C6U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0D0U, 2, 0x0003U)); // N0W0E + inside
    REQUIRE(mmio.write(vdp2 + 0x0200U, 2, 0x7C00U)); // blue BACK
    REQUIRE(mmio.write(vdp2 + 0x0002U, 2, 0x001FU)); // red NBG0 at x=1
    REQUIRE(mmio.write(vdp2 + 0x0004U, 2, 0x001FU)); // red NBG0 at x=2

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0] == 0xFF0000FFU);
    REQUIRE(frame.pixels[1U] == 0xFFFF0000U);
    REQUIRE(frame.pixels[2U] == 0xFFFF0000U);
    REQUIRE(frame.pixels[3U] == 0xFF0000FFU);
}

void appliesVdp2InvertedWindowToNbg0() {
    platform::MmioInterceptor mmio;
    const auto vdp2 = platform::VideoRuntime::kVdp2Base;
    const auto regs = platform::VideoRuntime::kVdp2Registers;
    REQUIRE(mmio.write(regs + 0x000U, 2, 0x8000U)); // display
    REQUIRE(mmio.write(regs + 0x020U, 2, 0x0001U)); // NBG0
    REQUIRE(mmio.write(regs + 0x028U, 2, 0x0042U)); // 32-bit RGB bitmap, 512x256
    REQUIRE(mmio.write(regs + 0x02CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x03CU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0ACU, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0AEU, 2, 0x0100U)); // keep BACK away from NBG0 data
    REQUIRE(mmio.write(regs + 0x0C0U, 2, 0U));
    REQUIRE(mmio.write(regs + 0x0C2U, 2, 8U));
    REQUIRE(mmio.write(regs + 0x0C4U, 2, 0x027FU));
    REQUIRE(mmio.write(regs + 0x0C6U, 2, 0x00E7U));
    // N0W0E with W0A clear: publish NBG0 outside W0. With the rectangle
    // starting at y=8, only the first eight scanlines should remain visible.
    REQUIRE(mmio.write(regs + 0x0D0U, 2, 0x0002U));
    for (std::size_t row = 0; row < 9U; ++row) {
        REQUIRE(mmio.write(vdp2 + static_cast<core::Address>(row * 2048U),
                           4, 0x80402010U));
    }

    mmio.video().advanceFrame();
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF102040U);
    REQUIRE(frame.pixels[7U * platform::VideoRuntime::kFrameWidth] == 0xFF102040U);
    REQUIRE(frame.pixels[8U * platform::VideoRuntime::kFrameWidth] == 0xFF000000U);
}

void interpretsVdp1CommandTable() {
    platform::MmioInterceptor mmio;
    const auto vram = platform::VideoRuntime::kVdp1Base;
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    const auto writeWord = [&mmio, vram](std::uint32_t offset, std::uint16_t value) {
        REQUIRE(mmio.write(vram + offset, 2, value));
    };
    writeWord(0x00U, 0x0004U); // polygon
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    writeWord(0x06U, 0x801FU); // mixed-mode RGB555 red
    writeWord(0x0CU, 1U); writeWord(0x0EU, 2U);
    writeWord(0x10U, 3U); writeWord(0x12U, 2U);
    writeWord(0x14U, 3U); writeWord(0x16U, 4U);
    writeWord(0x18U, 1U); writeWord(0x1AU, 4U);
    writeWord(0x20U, 0x8000U); // end
    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().vdp1CommandStats().commands == 1U);
    REQUIRE(mmio.video().vdp1CommandStats().polygons == 1U);
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[2U * platform::VideoRuntime::kFrameWidth + 1U] == 0xFFFF0000U);
}

void executesAutomaticVdp1CommandTableAtFrameBoundary() {
    platform::MmioInterceptor mmio;
    const auto vram = platform::VideoRuntime::kVdp1Base;
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    const auto writeWord = [&mmio, vram](std::uint32_t offset, std::uint16_t value) {
        REQUIRE(mmio.write(vram + offset, 2, value));
    };
    writeWord(0x00U, 0x0004U); // polygon
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    writeWord(0x06U, 0x801FU); // mixed-mode RGB555 red
    writeWord(0x0CU, 1U); writeWord(0x0EU, 2U);
    writeWord(0x10U, 3U); writeWord(0x12U, 2U);
    writeWord(0x14U, 3U); writeWord(0x16U, 4U);
    writeWord(0x18U, 1U); writeWord(0x1AU, 4U);
    writeWord(0x20U, 0x8000U); // end

    REQUIRE(mmio.write(registers + 0x04U, 2, 2U));
    REQUIRE(mmio.video().frameCount() == 0U);
    REQUIRE(mmio.video().vdp1CommandStats().commands == 0U);
    REQUIRE(!mmio.video().takeVdp1DrawEnd());

    mmio.video().advanceFrame();
    REQUIRE(mmio.video().frameCount() == 1U);
    REQUIRE(mmio.video().vdp1CommandStats().commands == 1U);
    REQUIRE(mmio.video().vdp1CommandStats().polygons == 1U);
    REQUIRE(mmio.video().takeVdp1DrawEnd());
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[2U * platform::VideoRuntime::kFrameWidth + 1U] == 0xFFFF0000U);

    mmio.video().advanceFrame();
    REQUIRE(mmio.video().frameCount() == 2U);
    REQUIRE(mmio.video().vdp1CommandStats().commands == 1U);
    REQUIRE(mmio.video().takeVdp1DrawEnd());
}

void rendersVdp1TexturedSpriteWithClutAndClipping() {
    platform::MmioInterceptor mmio;
    const auto vram = platform::VideoRuntime::kVdp1Base;
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    const auto writeWord = [&mmio, vram](std::uint32_t offset, std::uint16_t value) {
        REQUIRE(mmio.write(vram + offset, 2, value));
    };

    // Four-bit lookup texture: one nonzero texel followed by transparent
    // texels. CMD_COLR=0x20 selects the CLUT at VRAM+0x100.
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    writeWord(0x100U + 2U, 0x83E0U); // palette index 1 = RGB green
    writeWord(0x200U, 0x1111U);      // packed nibbles: 1,1,1,1

    // System clipping command (2..3 in both axes).
    writeWord(0x000U, 0x0009U);
    writeWord(0x00CU, 2U); writeWord(0x00EU, 2U);
    writeWord(0x010U, 3U); writeWord(0x012U, 3U);
    writeWord(0x014U, 3U); writeWord(0x016U, 3U);
    // Scaled 8x1 lookup sprite at (1,1), stretched to A=(1,1), C=(5,5).
    writeWord(0x020U, 0x0001U);
    writeWord(0x024U, 0x0008U); // 4-bit lookup mode
    writeWord(0x026U, 0x0020U); // CLUT address
    writeWord(0x028U, 0x0040U); // source = VRAM+0x200
    writeWord(0x02AU, 0x0101U); // source width 8, height 1
    writeWord(0x02CU, 1U); writeWord(0x02EU, 1U);
    writeWord(0x030U, 5U); writeWord(0x032U, 1U);
    writeWord(0x034U, 5U); writeWord(0x036U, 5U);
    writeWord(0x038U, 1U); writeWord(0x03AU, 5U);
    writeWord(0x040U, 0x8000U);

    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(mmio.video().vdp1CommandStats().sprites == 1U);
    REQUIRE(frame.pixels[2U * platform::VideoRuntime::kFrameWidth + 2U] ==
            0xFF00FF00U);
    REQUIRE(frame.pixels[1U * platform::VideoRuntime::kFrameWidth + 1U] ==
            0xFF000000U);
    REQUIRE(frame.pixels[3U * platform::VideoRuntime::kFrameWidth + 3U] ==
            0xFF00FF00U);
    REQUIRE(frame.pixels[3U * platform::VideoRuntime::kFrameWidth + 4U] ==
            0xFF000000U);
}

void rendersVulkanVdp1AcrossFullDisplayHeight() {
    std::string error;
    auto renderer = platform::createVulkanVdp1(error);
    if (!renderer) {
        std::cout << "[skip] Vulkan VDP1 height comparison: " << error << '\n';
        return;
    }
    const auto render = [](std::shared_ptr<platform::Vdp1ComputeRenderer> backend,
                           std::uint16_t tvmd, std::uint16_t top,
                           std::uint16_t height) {
        platform::VideoRuntime video;
        video.setVdp1Renderer(std::move(backend));
        const auto vram = platform::VideoRuntime::kVdp1Base;
        const auto regs = platform::VideoRuntime::kVdp1Registers;
        const auto vdp2 = platform::VideoRuntime::kVdp2Registers;
        const auto write = [&video](core::Address address, std::uint16_t value) {
            REQUIRE(video.write(address, 2U, value));
        };
        write(vdp2, tvmd);
        write(vdp2 + 0x0E0U, 0x0025U);
        write(vdp2 + 0x0F0U, 0x0001U);
        write(vram + 0x004U, 0x0028U); // RGB555 sprite
        write(vram + 0x008U, 0x0040U); // texture at VRAM+0x200
        write(vram + 0x00AU, static_cast<std::uint16_t>(0x0100U | height));
        write(vram + 0x00CU, 10U);
        write(vram + 0x00EU, top);
        write(vram + 0x020U, 0x8000U); // end of command list
        for (std::uint32_t i = 0; i < 8U * height; ++i) {
            write(vram + 0x200U + i * 2U, 0x801FU);
        }
        write(regs + 0x04U, 1U);
        platform::VideoFrame frame;
        REQUIRE(video.consumeFrame(frame));
        return frame;
    };
    for (const auto [tvmd, top, height, lastRow] : {
             std::array<std::uint16_t, 4>{0x8000U, 208U, 16U, 223U},
             std::array<std::uint16_t, 4>{0x8010U, 220U, 24U, 239U},
             std::array<std::uint16_t, 4>{0x8020U, 240U, 16U, 255U}}) {
        const auto cpu = render(nullptr, tvmd, top, height);
        const auto gpu = render(renderer, tvmd, top, height);
        REQUIRE(cpu.height == static_cast<std::uint32_t>(lastRow) + 1U);
        REQUIRE(cpu.pixels[static_cast<std::size_t>(lastRow) * 320U + 10U] == 0xFFFF0000U);
        REQUIRE(cpu.pixels == gpu.pixels);
    }
    REQUIRE(renderer->dispatchCount() >= 3U);
}

void rendersVdp1EightBitBankSpriteFromCram() {
    platform::MmioInterceptor mmio;
    const auto vram = platform::VideoRuntime::kVdp1Base;
    const auto cram = platform::VideoRuntime::kVdp2Cram;
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    const auto writeVram = [&mmio, vram](std::uint32_t offset, std::uint16_t value) {
        REQUIRE(mmio.write(vram + offset, 2, value));
    };

    // Mode 4 is an 8-bit/256-color bank mode. CMDCOLR selects entries 0x100+
    // index in VDP2 CRAM; index zero remains transparent.
    REQUIRE(mmio.write(cram + 0x0302U, 2, 0x001FU)); // entry 0x181 = red
    REQUIRE(mmio.write(cram + 0x0304U, 2, 0x03E0U)); // entry 0x182 = green
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    writeVram(0x000U, 0x0000U); // fixed-size normal sprite
    writeVram(0x004U, 0x0020U); // mode 4
    writeVram(0x006U, 0x0100U); // color bank 0x100
    writeVram(0x008U, 0x0040U); // source = VRAM+0x200
    writeVram(0x00AU, 0x0101U); // X field 1 => 8 px; Y field 1 => 1 px
    writeVram(0x00CU, 0U); writeVram(0x00EU, 0U);
    // B/C/D are ignored by a normal sprite; deliberately set coordinates
    // that would create a large stretched rectangle in an incorrect renderer.
    writeVram(0x010U, 100U); writeVram(0x012U, 100U);
    writeVram(0x014U, 200U); writeVram(0x016U, 200U);
    writeVram(0x018U, 300U); writeVram(0x01AU, 300U);
    writeVram(0x200U, 0x8182U); // two texels: 0x81, 0x82
    writeVram(0x020U, 0x8000U); // end

    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);
    REQUIRE(frame.pixels[1U] == 0xFF00FF00U);
    REQUIRE(frame.pixels[20U * platform::VideoRuntime::kFrameWidth + 20U] ==
            0xFF000000U);
}

void decodesVdp1CharacterSizeXAndYFields() {
    platform::MmioInterceptor mmio;
    const auto vram = platform::VideoRuntime::kVdp1Base;
    const auto cram = platform::VideoRuntime::kVdp2Cram;
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    const auto writeWord = [&mmio, vram](std::uint32_t offset, std::uint16_t value) {
        REQUIRE(mmio.write(vram + offset, 2U, value));
    };

    REQUIRE(mmio.write(cram + 2U, 2U, 0x001FU)); // palette entry 1 = red
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2U, 0x0001U));
    writeWord(0x000U, 0x0000U); // fixed-size normal sprite
    writeWord(0x004U, 0x0000U); // 4-bit color-bank mode
    writeWord(0x006U, 0x0000U); // color bank 0
    writeWord(0x008U, 0x0040U); // source = VRAM+0x200
    writeWord(0x00AU, 0x0210U); // X=2 => 16 px; Y=0x10 => 16 px
    writeWord(0x00CU, 0U);
    writeWord(0x00EU, 0U);
    writeWord(0x200U + 126U, 0x0001U); // last texel of the 16x16 pattern
    writeWord(0x020U, 0x8000U); // end

    REQUIRE(mmio.write(registers + 0x04U, 2U, 1U));
    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[15U * platform::VideoRuntime::kFrameWidth + 15U] ==
            0xFFFF0000U);
    REQUIRE(frame.pixels[1U * platform::VideoRuntime::kFrameWidth + 127U] ==
            0xFF000000U);
}

void resolvesIndirectTargetOnDemand() {
    // JMP @R3; NOP; NOP; NOP; TRAPA #0x12.
    // Static CFG discovery reaches only 0x100. R3 resolves 0x108 at runtime.
    const auto image = test::imageFromWords(0x100U, {
        0x432BU, 0x0009U, 0x0009U, 0x0009U, 0xC312U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    REQUIRE(cfg.blockAt(0x108U) == nullptr);

    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());
    sh2::RuntimeBlockResolver resolver(image, module);

    sir::MachineState state;
    state.writeState(sh2::stateSlot(sh2::Register::R3), 0x108U);
    sir::ProgramExecutionOptions options;
    options.maxSteps = 4;
    options.resolveBlock = [&resolver](core::Address address) { return resolver.resolve(address); };
    const auto result = sir::ProgramExecutor{}.run(module.blockAt(0x100U), state, options);
    REQUIRE(result.status == sir::ProgramStatus::Trap);
    REQUIRE(result.last.trapVector == 0x12U);
    REQUIRE(result.steps == 2);
    REQUIRE(resolver.stats().requests == 1);
    REQUIRE(resolver.stats().loweredBlocks == 1);
    REQUIRE(module.blockAt(0x108U) != nullptr && module.blockAt(0x108U)->isLowered());
}

void executesCodeLoadedAfterBootImageDiscovery() {
    const auto image = test::imageFromWords(0x100U, {0x432BU, 0x0009U, 0xC312U});
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());
    sir::MachineState state;
    sh2::RuntimeBlockResolver resolver(image, module, &state);
    REQUIRE(resolver.resolve(0x00220000U) == nullptr);
    // Simulate a later CD load. Its PC-relative literal lies outside the
    // first recovered block; the untaken gap contains unsupported data.
    state.write16BE(0x00220000U, 0xD107U); // MOV.L @(PC+0x1C),R1
    state.write16BE(0x00220002U, 0xE007U); // MOV #7,R0
    state.write16BE(0x00220004U, 0xA004U); // BRA 0x00220010
    state.write16BE(0x00220006U, 0x7001U); // ADD #1,R0 (delay slot)
    state.write16BE(0x00220008U, 0xFFFFU);
    state.write16BE(0x00220010U, 0x2102U); // MOV.L R0,@R1
    state.write16BE(0x00220012U, 0x000BU); // RTS to static image
    state.write16BE(0x00220014U, 0x0009U);
    state.write32BE(0x00220020U, 0x06080000U);
    state.writeState(sh2::stateSlot(sh2::Register::R3), 0x00220000U);
    state.writeState(sh2::stateSlot(sh2::Register::PR), 0x104U);
    sir::ProgramExecutionOptions options;
    options.maxSteps = 8;
    options.resolveBlock = [&resolver](core::Address address) { return resolver.resolve(address); };
    const auto result = sir::ProgramExecutor{}.run(module.blockAt(0x100U), state, options);
    REQUIRE(result.status == sir::ProgramStatus::Trap);
    REQUIRE(result.last.trapVector == 0x12U);
    REQUIRE(state.read32BE(0x06080000U) == 8U);
    REQUIRE(module.blockAt(0x00220008U) == nullptr);
    REQUIRE(resolver.stats().loweredBlocks == 3U);
}

void recordsRuntimeOverlayReplacementAtResolutionStep() {
    const auto image = test::imageFromWords(0x100U, {
        0x432BU, 0x0009U, // JMP @R3 with delay-slot NOP
        0xC310U,          // old overlay body: TRAPA #0x10
    });
    const auto cfg = sh2::BlockBuilder{}.build(
        image, std::vector<core::Address>{0x100U, 0x104U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());

    sir::MachineState state;
    state.writeState(sh2::stateSlot(sh2::Register::R3), 0x104U);
    sh2::RuntimeBlockResolver resolver(image, module, &state);
    sir::ProgramExecutionOptions options;
    options.maxSteps = 4U;
    options.refreshKnownBlocksOnWrite = true;
    options.resolveBlockAtStep = [&resolver](core::Address address, std::size_t step) {
        return resolver.resolve(address, step);
    };
    options.beforeBlock = [](const sir::BasicBlock& block, sir::MachineState& machine,
                             std::size_t step) {
        if (block.address() == 0x100U && step == 0U) {
            machine.write16BE(0x104U, 0xC312U); // replacement: TRAPA #0x12
        }
    };

    const auto result = sir::ProgramExecutor{}.run(module.blockAt(0x100U), state, options);
    REQUIRE(result.status == sir::ProgramStatus::Trap);
    REQUIRE(result.last.trapVector == 0x12U);
    REQUIRE(result.steps == 2U);
    REQUIRE(resolver.replacements().size() == 1U);
    const auto& replacement = resolver.replacements().front();
    REQUIRE(replacement.address == 0x104U);
    REQUIRE(replacement.step == 1U);
    REQUIRE(replacement.previousSignatureKnown);
    REQUIRE(replacement.previousSignature != replacement.replacementSignature);
}

void interceptsSaturnMmioWithoutTouchingRam() {
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    const auto address = builder.constI32(static_cast<std::int32_t>(0x05E00000U));
    builder.store32(address, builder.constI32(0x11223344));
    builder.writeState(sh2::stateSlot(sh2::Register::R0), builder.load32(address));
    builder.trap(0x7FU);

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    state.setMemoryInterceptor(&mmio);
    const auto result = sir::ProgramExecutor{}.run(&entry, state);
    REQUIRE(result.status == sir::ProgramStatus::Trap);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R0)) == 0x11223344U);
    REQUIRE(state.memory().empty());
    REQUIRE(mmio.accesses().size() == 2);
    REQUIRE(mmio.accesses()[0].write);
    REQUIRE(!mmio.accesses()[1].write);
    REQUIRE(mmio.accesses()[0].space == platform::AddressSpace::Vdp2);
    REQUIRE(mmio.accesses()[1].value == 0x11223344U);
    REQUIRE(mmio.accesses()[0].context.blockAddress == 0x100U);
    REQUIRE(mmio.accesses()[0].context.step == 0U);

    mmio.setAccessRecording(false);
    state.write32BE(0x05E00000U, 0x55667788U);
    REQUIRE(state.read32BE(0x05E00000U) == 0x55667788U);
    REQUIRE(mmio.accesses().empty());
}

void boundsRetainedMmioAccessHistory() {
    platform::MmioInterceptor mmio;
    mmio.setAccessHistoryLimit(2U);

    REQUIRE(mmio.write(0x05E00000U, 4U, 0x11111111U));
    REQUIRE(mmio.write(0x05E00000U, 4U, 0x22222222U));
    REQUIRE(mmio.write(0x05E00000U, 4U, 0x33333333U));

    REQUIRE(mmio.accessCount() == 3U);
    REQUIRE(mmio.accesses().size() == 2U);
    REQUIRE(mmio.accesses()[0].value == 0x22222222U);
    REQUIRE(mmio.accesses()[1].value == 0x33333333U);

    mmio.clearAccesses();
    REQUIRE(mmio.accessCount() == 0U);
    REQUIRE(mmio.accesses().empty());
}

void presentsOnlyAtSchedulerFrameBoundary() {
    platform::MmioInterceptor mmio;
    std::size_t presentations{};
    std::uint64_t lastSequence{};
    mmio.video().setFrameSink([&](const platform::VideoFrame& frame) {
        ++presentations;
        lastSequence = frame.sequence;
    });
    const auto registers = platform::VideoRuntime::kVdp1Registers;
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp1Framebuffer, 2, 0x801FU));
    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    REQUIRE(presentations == 0U);
    REQUIRE(mmio.video().frameCount() == 1U);

    sir::Module module;
    auto& block = module.createBlock(0x06010000U, "presentation-boundary");
    sir::MachineState state;
    platform::ScuEventScheduler scheduler(1U);
    mmio.attachMachineState(state);
    REQUIRE(!scheduler.beforeBlock(block, state, 0U, mmio).has_value());
    REQUIRE(presentations == 1U);
    REQUIRE(lastSequence == 2U);
}

void capturesVideoDiagnosticSnapshotAtPresentationBoundary() {
    platform::MmioInterceptor mmio;
    platform::VideoDiagnosticSnapshot snapshot;
    std::size_t captures{};
    mmio.video().setDiagnosticSink([&](const platform::VideoDiagnosticSnapshot& value) {
        snapshot = value;
        ++captures;
    });

    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers, 2, 0x8000U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp1Framebuffer, 2, 0x801FU));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp1Registers + 0x04U, 2, 1U));
    REQUIRE(captures == 0U);

    mmio.video().presentFrame();
    REQUIRE(captures == 1U);
    REQUIRE(snapshot.composed.sequence == 1U);
    REQUIRE(snapshot.vdp1Banks[0].pixels[0] == 0xFFFF0000U);
    REQUIRE(snapshot.vdp1Banks[1].pixels[0] == 0xFF000000U);
    REQUIRE(snapshot.vdp1Bounds[0].nonZeroPixels == 1U);
    REQUIRE(snapshot.vdp1Bounds[0].minX == 0U);
    REQUIRE(snapshot.vdp1Bounds[0].minY == 0U);
    REQUIRE(snapshot.vdp1Memory.nonZeroBytes == 2U);
    REQUIRE(snapshot.vdp2Bounds.nonZeroPixels == 0U);
    REQUIRE(snapshot.composedBounds.nonZeroPixels == 1U);
    REQUIRE(snapshot.vdp1Ptmr == 1U);
    REQUIRE(snapshot.vdp2Tvmd == 0x8000U);
}

void swapsVdp1ManualFramebufferAtPresentation() {
    platform::MmioInterceptor mmio;
    const auto framebuffer = platform::VideoRuntime::kVdp1Framebuffer;
    const auto registers = platform::VideoRuntime::kVdp1Registers;

    // FBCR=FCM|FCT=3 enables manual change mode. The first draw goes to the
    // side opposite the currently displayed bank, and only presentation
    // makes that side visible.
    REQUIRE(mmio.write(registers + 0x02U, 2, 0x0003U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0E0U, 2, 0x0025U));
    REQUIRE(mmio.write(platform::VideoRuntime::kVdp2Registers + 0x0F0U, 2, 0x0001U));
    REQUIRE(mmio.write(framebuffer, 2, 0x83E0U)); // green, draw bank 1
    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().framePending());
    mmio.video().presentFrame();

    platform::VideoFrame frame;
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFF00FF00U);

    // The next PTMR draw targets bank zero while bank one remains displayed;
    // the second presentation therefore reveals the new red frame.
    REQUIRE(mmio.write(framebuffer, 2, 0x801FU));
    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    mmio.video().presentFrame();
    REQUIRE(mmio.video().consumeFrame(frame));
    REQUIRE(frame.pixels[0U] == 0xFFFF0000U);
}

void capturesReenabledBlackCellFrame() {
    platform::VideoRuntime video;
    platform::VideoDiagnosticSnapshot snapshot;
    unsigned captures = 0;
    video.captureFirstReenabledBlackFrame([&](const auto& value) {
        snapshot = value;
        ++captures;
    });
    const auto present = [&] { video.advanceFrame(); video.presentFrame(); };
    REQUIRE(video.write(video.kVdp2Tvmd, 2, 0x8010));
    present(); // Initial black must not trigger.
    REQUIRE(captures == 0);
    REQUIRE(video.write(video.kVdp2Base, 2, 0x001F)); // Visible BACK.
    present();
    REQUIRE(video.lastFrame().pixels[0] == 0xFFFF0000U);
    REQUIRE(video.write(video.kVdp2Tvmd, 2, 0x0010));
    present();
    REQUIRE(video.write(video.kVdp2Base, 2, 0));
    REQUIRE(video.write(video.kVdp2Bgon, 2, 1));
    REQUIRE(video.write(video.kVdp2Pncn0, 2, 0x8000));
    REQUIRE(video.write(video.kVdp2Mpabn0, 2, 1)); // Map starts at 0x2000.
    REQUIRE(video.write(video.kVdp2Base + 31, 2, 0)); // Cross 32-byte boundary.
    REQUIRE(video.write(video.kVdp2Tvmd, 2, 0x8010));
    present();
    REQUIRE(captures == 1);
    REQUIRE(snapshot.composed.sequence == 4);
    REQUIRE(snapshot.composed.width == 320U);
    REQUIRE(snapshot.composed.height == 240U); // TVMD.VRESO=01
    REQUIRE(snapshot.cellSamples.size() == 320U * 240U);
    REQUIRE(snapshot.cellSamples[0].mapAddress == 0x2000);
    REQUIRE(snapshot.cellSamples[0].characterAddress == 0);
    REQUIRE(snapshot.cellSamples[0].cramAddress == 0);
    REQUIRE(snapshot.cellSamples[0].color == 0);
    REQUIRE(snapshot.vramWrites[0].writes == 3);
    REQUIRE(snapshot.vramWrites[1].writes == 1);
    REQUIRE(snapshot.vramWrites[1].lastFrame == 3);
    REQUIRE(snapshot.vramDump.size() == video.kVdp2VramSize);
    REQUIRE(snapshot.composedBounds.nonZeroPixels == 0);
    present();
    REQUIRE(captures == 1);
}

void dispatchesScuTimer0AtConfiguredScanline() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address handlerAddress = 0x200U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "timer0-entry");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);

    platform::MmioInterceptor mmio;
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.setMemoryInterceptor(&mmio);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), 0U);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(platform::ScuEventScheduler::kScuInterruptTable +
                        platform::ScuEventScheduler::kTimer0Vector * 4U,
                    handlerAddress);
    // Keep HBlank pending but masked so Timer 0 is the factor selected at the
    // same scanline edge.
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4,
                       platform::ScuEventScheduler::kHBlankInStatus));
    REQUIRE(mmio.write(0x25FE0090U, 4, 1U));
    REQUIRE(mmio.write(0x25FE0098U, 4, 0x00000100U));
    REQUIRE(mmio.scuTimer0Compare() == 1U);
    REQUIRE(mmio.scuTimer0Enabled());

    platform::ScuEventScheduler scheduler(526U); // two blocks per scanline
    REQUIRE(!scheduler.beforeBlock(entry, state, 0U, mmio).has_value());
    const auto delivered = scheduler.beforeBlock(entry, state, 1U, mmio);
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->target == handlerAddress);
    REQUIRE(delivered->returnMode == sir::InterruptReturnMode::Callback);
    REQUIRE(scheduler.stats().timer0Events == 1U);
    REQUIRE((mmio.scuInterruptStatus() &
             platform::ScuEventScheduler::kTimer0Status) == 0U);
}

void executesSh2DivisionUnitWindow() {
    platform::MmioInterceptor mmio;
    constexpr core::Address divisor = 0xFFFFFF00U;
    constexpr core::Address dividendLow = 0xFFFFFF04U;
    constexpr core::Address dividendHigh = 0xFFFFFF08U;
    constexpr core::Address quotient = 0xFFFFFF14U;

    // This is the exact calculation used by the GFS boot path for a
    // 0x2800-byte root directory: (size + 0x7ff) / 0x800 = 5 sectors.
    REQUIRE(mmio.write(divisor, 4, 0x800U));
    REQUIRE(mmio.write(dividendHigh, 4, 0U));
    REQUIRE(mmio.write(dividendLow, 4, 0x2FFFU));
    REQUIRE(mmio.read(quotient, 4) == 5U);
    REQUIRE(mmio.read(divisor, 4) == 0x800U);
    REQUIRE(mmio.read(dividendLow, 4) == 0x2FFFU);
}

void advancesSh2FreeRunningTimer() {
    platform::MmioInterceptor mmio;
    const auto counter = platform::MmioInterceptor::kSh2FreeRunningCounter;
    const auto control = platform::MmioInterceptor::kSh2FreeRunningTimerControl;

    mmio.setExecutionContext({0x100U, 0U, 0U, {}});
    mmio.setExecutionContext({0x100U, 0x100U, 0x123U, {}});
    REQUIRE(mmio.read(counter, 2) == 0x0123U);
    REQUIRE(mmio.read(counter, 1) == 0x01U);
    REQUIRE(mmio.read(counter + 1U, 1) == 0x23U);

    REQUIRE(mmio.write(counter, 2, 0xFFF0U));
    mmio.setExecutionContext({0x100U, 0x100U, 0x143U, {}});
    REQUIRE(mmio.read(counter, 2) == 0x0010U);
    REQUIRE((mmio.read(counter - 1U, 1).value_or(0U) & 0x02U) != 0U);

    // TCR.CKS=1 divides the deterministic block clock by four.
    REQUIRE(mmio.write(control, 1, 1U));
    REQUIRE(mmio.write(counter, 2, 0U));
    mmio.setExecutionContext({0x100U, 0x100U, 0x14AU, {}});
    REQUIRE(mmio.read(counter, 2) == 1U);
    mmio.setExecutionContext({0x100U, 0x100U, 0x14BU, {}});
    REQUIRE(mmio.read(counter, 2) == 2U);
}

void synchronizesSh2TimerWithDisplayBudget() {
    platform::MmioInterceptor split;
    platform::MmioInterceptor whole;
    split.setCpuTiming(26'874'100U, 600'000U);
    whole.setCpuTiming(26'874'100U, 1'200'000U);
    const auto counter = platform::MmioInterceptor::kSh2FreeRunningCounter;
    const auto control = platform::MmioInterceptor::kSh2FreeRunningTimerControl;
    for (unsigned prescaler : {0U, 1U, 2U}) {
        REQUIRE(split.write(control, 1U, prescaler));
        REQUIRE(whole.write(control, 1U, prescaler));
        REQUIRE(split.write(counter, 2U, 0U));
        REQUIRE(whole.write(counter, 2U, 0U));
        split.setExecutionContext({0x100U, 0U, 0U, {}});
        whole.setExecutionContext({0x100U, 0U, 0U, {}});
        for (std::size_t step = 1U; step <= 600'000U; step += 997U) {
            split.setExecutionContext({0x100U, 0U, step, {}});
        }
        split.setExecutionContext({0x100U, 0U, 600'000U, {}});
        whole.setExecutionContext({0x100U, 0U, 1'200'000U, {}});
        const auto divisor = prescaler == 0U ? 8U : prescaler == 1U ? 32U : 128U;
        const auto expected = (26'874'100U / divisor) & 0xFFFFU;
        REQUIRE(split.read(counter, 2U) == expected);
        REQUIRE(whole.read(counter, 2U) == expected);
        REQUIRE((split.read(counter - 1U, 1U).value_or(0U) & 2U) != 0U);
    }
    REQUIRE(split.write(control, 1U, 3U)); // External clock does not follow CPU time.
    const auto before = split.read(counter, 2U);
    split.setExecutionContext({0x100U, 0U, 1'200'000U, {}});
    REQUIRE(split.read(counter, 2U) == before);
}

void executesSh2WordMultiply() {
    const auto image = test::imageFromWords(0x100U, {0x246EU, 0x000BU, 0x0009U});
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    sir::Module module;
    REQUIRE(sh2::lowerCFG(cfg, module).success());
    sir::MachineState state;
    state.writeState(sh2::stateSlot(sh2::Register::R4), 2U);
    state.writeState(sh2::stateSlot(sh2::Register::R6), 0xFFFFU);
    const auto result = sir::Interpreter{}.execute(*module.blockAt(0x100U), state);
    REQUIRE(result.status == sir::InterpreterStatus::Success);
    REQUIRE(result.control == sir::ControlKind::Return);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::MACL)) == 0x1FFFEU);
}

void handlesCacheBypassingPeripheralAliases() {
    constexpr core::Address alias = 0x20000000U;
    platform::MmioInterceptor mmio;

    const auto framebuffer = platform::VideoRuntime::kVdp1Framebuffer + alias;
    const auto registers = platform::VideoRuntime::kVdp1Registers + alias;
    REQUIRE(mmio.write(framebuffer, 2, 0x001FU));
    REQUIRE(mmio.write(registers + 0x04U, 2, 1U));
    REQUIRE(mmio.video().frameCount() == 1U);

    const auto cdHirq = platform::CdRuntime::kCommandBase + alias + 0x08U;
    REQUIRE(mmio.read(cdHirq, 2) == platform::CdRuntime::kHirqCmok);
    REQUIRE(mmio.accesses()[0].space == platform::AddressSpace::Vdp1);
    REQUIRE(mmio.accesses()[2].space == platform::AddressSpace::CdBlock);
}

void mapsScspWindowAndServicesMailbox() {
    platform::MmioInterceptor mmio;
    REQUIRE(platform::classify(platform::ScspRuntime::kRamBase) ==
            platform::AddressSpace::Scsp);
    REQUIRE(platform::classify(platform::ScspRuntime::kRegistersBase) ==
            platform::AddressSpace::Scsp);
    REQUIRE(platform::classify(platform::ScspRuntime::kRegistersBase + 0x20000000U) ==
            platform::AddressSpace::Scsp);

    REQUIRE(mmio.write(platform::ScspRuntime::kRamBase + 0x20U, 2, 0x1234U));
    REQUIRE(mmio.read(platform::ScspRuntime::kRamBase + 0x20U, 2) == 0x1234U);
    REQUIRE(mmio.write(platform::ScspRuntime::kRegistersBase + 0x400U, 2, 0xABCDU));
    REQUIRE(mmio.read(platform::ScspRuntime::kRegistersBase + 0x400U, 2) == 0xABCDU);
    REQUIRE(mmio.read(platform::ScspRuntime::kRegistersBase + 0x20000000U + 0x400U,
                      2) == 0xABCDU);
    REQUIRE(mmio.write(platform::ScspRuntime::kCommandStatus, 1, 0x80U));
    REQUIRE(mmio.read(platform::ScspRuntime::kCommandStatus, 1) == 0x80U);

    sir::Module module;
    auto& block = module.createBlock(0x06010000U, "scsp-mailbox");
    sir::MachineState state;
    mmio.attachMachineState(state);
    state.write16BE(platform::ScspRuntime::kRequest0, 1U);
    state.write16BE(platform::ScspRuntime::kRequest1, 1U);

    platform::ScuEventScheduler scheduler;
    REQUIRE(!scheduler.beforeBlock(block, state, 0U, mmio).has_value());
    REQUIRE(mmio.read(platform::ScspRuntime::kCommandStatus, 1) == 0U);
    REQUIRE(state.read16BE(platform::ScspRuntime::kSequence) == 1U);
    REQUIRE(state.read16BE(platform::ScspRuntime::kReady0) == 1U);
    REQUIRE(state.read16BE(platform::ScspRuntime::kReady1) == 1U);
    REQUIRE(mmio.scsp().stats().requests == 1U);
    REQUIRE(mmio.scsp().stats().completions == 2U);

    // A level-triggered request is acknowledged once; clearing both request
    // words rearms the producer for the next command.
    REQUIRE(!scheduler.beforeBlock(block, state, 1U, mmio).has_value());
    REQUIRE(mmio.scsp().stats().requests == 1U);

    // The game can hold a request high across commands and consume only its
    // ready flag. That cleared flag rearms the corresponding channel.
    state.write16BE(platform::ScspRuntime::kReady1, 0U);
    REQUIRE(!scheduler.beforeBlock(block, state, 2U, mmio).has_value());
    REQUIRE(state.read16BE(platform::ScspRuntime::kSequence) == 2U);
    REQUIRE(state.read16BE(platform::ScspRuntime::kReady1) == 1U);
    REQUIRE(mmio.scsp().stats().requests == 2U);

    state.write16BE(platform::ScspRuntime::kRequest0, 0U);
    state.write16BE(platform::ScspRuntime::kRequest1, 0U);
    REQUIRE(!scheduler.beforeBlock(block, state, 3U, mmio).has_value());
    state.write16BE(platform::ScspRuntime::kRequest0, 1U);
    REQUIRE(!scheduler.beforeBlock(block, state, 4U, mmio).has_value());
    REQUIRE(state.read16BE(platform::ScspRuntime::kSequence) == 3U);
    REQUIRE(mmio.scsp().stats().requests == 3U);
}

void consumesPcmCommandsAndPublishesPlaybackPosition() {
    platform::ScspRuntime sound;
    sir::MachineState state;
    constexpr auto ram = platform::ScspRuntime::kRamBase;
    constexpr auto host = 0x900U; // Relocated interface, not hard-coded 0x700.
    const auto put = [&](std::uint32_t offset, std::uint8_t width, std::uint32_t value) {
        REQUIRE(sound.write(ram + offset, width, value));
    };
    const auto get = [&](std::uint32_t offset) { return sound.read(ram + offset, 1U); };
    const auto submit = [&] {
        put(0x4E0U, 1U, 0x80U);
        sound.onSchedulerTick(state);
        REQUIRE(get(0x4E0U) == 0U);
        REQUIRE(get(host) == 0U);
    };
    put(0x404U, 4U, host);
    put(host, 4U, 0x850090E0U); // stereo, signed 8-bit, stream 0, full volume
    put(host + 4U, 4U, 0x10002000U); // start 0x10000, 8192 samples/channel
    put(host + 8U, 2U, 0U); // 44100 Hz
    put(0x10000U, 1U, 0x40U);
    put(0x12000U, 1U, 0xC0U);
    put(host + 0xA1U, 1U, 0x5AU); // unused status byte must survive
    submit();
    REQUIRE(sound.stats().pcmStarts == 1U);
    REQUIRE(get(host + 0xA0U) == 0U);
    sound.onVBlank();
    platform::ScspRuntime::AudioFrame frame;
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[0] == -0.5F);
    REQUIRE(frame.samples[1] == 0.5F);
    for (int i = 0; i < 4; ++i) sound.onVBlank();
    REQUIRE(get(host + 0xA0U) == 0U); // 3675 < 4096
    submit(); // Empty batch must not restart playback.
    sound.onVBlank();
    REQUIRE(get(host + 0xA0U) == 1U); // 4410 samples
    REQUIRE(sound.stats().pcmStarts == 1U);
    REQUIRE(get(host + 0xA1U) == 0x5AU);
    for (int i = 0; i < 6; ++i) sound.onVBlank();
    REQUIRE(get(host + 0xA0U) == 0U); // Wrapped at 8192, not at 65536.
    REQUIRE(sound.stats().pcmStatusUpdates == 2U);
    put(host, 4U, 0x86000000U);
    submit();
    for (int i = 0; i < 6; ++i) sound.onVBlank();
    REQUIRE(get(host + 0xA0U) == 0U);
    REQUIRE(sound.stats().activeVoices == 0U);

    put(host, 4U, 0x850001E0U); // mono, 16-bit, independent stream 1
    put(host + 4U, 4U, 0x10000000U); // zero size encodes 64K samples
    put(host + 8U, 2U, 0x7800U); // octave -1: 22050 Hz
    put(0x10000U, 2U, 0x8000U);
    submit();
    sound.onVBlank();
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[0] == -1.0F);
    REQUIRE(frame.samples[1] == -1.0F);
    for (int i = 0; i < 10; ++i) sound.onVBlank();
    REQUIRE(get(host + 0xA2U) == 0U);
    sound.onVBlank();
    REQUIRE(get(host + 0xA2U) == 1U);
    put(host, 4U, 0x850091E0U);
    put(host + 4U, 4U, 0x7FFF0000U); // stereo buffer outside sound RAM
    submit();
    sound.onVBlank();
    REQUIRE(sound.stats().activeVoices == 0U);
    REQUIRE(sound.stats().pcmStarts == 2U);

    // Movie setup commands must be consumed so the SH-2 queue can return to
    // its first packet instead of advancing into the host status area.
    put(host, 4U, 0x10000101U);
    put(host + 4U, 4U, 0x01010100U);
    submit();
    put(host, 4U, 0x850090E0U);
    put(host + 4U, 4U, 0x10002000U);
    put(host + 8U, 2U, 0x7800U);
    put(0x10000U, 1U, 0x40U);
    put(0x12000U, 1U, 0xC0U);
    submit();
    put(host, 4U, 0x10000100U); // Stop sequences only, preserve PCM.
    put(host + 4U, 4U, 0U);
    submit();
    sound.onVBlank();
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[0] == -0.5F);
    REQUIRE(frame.samples[1] == 0.5F);
    put(host, 4U, 0x82000000U); // Master mute must not freeze the cursor.
    submit();
    for (int i = 0; i < 11; ++i) sound.onVBlank();
    REQUIRE(get(host + 0xA0U) == 1U);
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[0] == 0.0F);
    put(host, 4U, 0x82000F00U);
    submit();
    put(host, 4U, 0x10000001U); // Stop PCM independently.
    submit();
    sound.onVBlank();
    REQUIRE(sound.stats().activeVoices == 0U);

    // Partial HLE must not acknowledge commands whose effects it cannot model.
    put(host, 4U, 0x08000000U);
    put(0x4E0U, 1U, 0x80U);
    sound.onSchedulerTick(state);
    REQUIRE(get(host) == 8U);
    put(0x404U, 4U, 0xFFFFFFFFU);
    put(0x4E0U, 1U, 0x80U);
    sound.onSchedulerTick(state);
    REQUIRE(get(host) == 8U);
    put(0x404U, 4U, host);
    put(host, 4U, 0x85001000U); // Muting output must not stop consumption.
    put(host + 4U, 4U, 0x10002000U);
    put(host + 8U, 2U, 0U);
    submit();
    for (int i = 0; i < 6; ++i) sound.onVBlank();
    REQUIRE(get(host + 0xA0U) == 1U);
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(std::all_of(frame.samples.begin(), frame.samples.end(),
                        [](float value) { return value == 0.0F; }));
}

void rendersScspAudioFrameAtVblank() {
    platform::MmioInterceptor mmio;
    const auto registers = platform::ScspRuntime::kRegistersBase;
    // Configure one deterministic voice and trigger it through the SCSP
    // key-on register. The exact Saturn envelope is outside this HLE, but the
    // host must receive a correctly sized stereo frame with nonzero samples.
    REQUIRE(mmio.write(registers + 0x00U, 2, 440U));
    REQUIRE(mmio.write(registers + 0x04U, 2, 32U));
    REQUIRE(mmio.write(registers + 0x08U, 2, 0x80U));
    REQUIRE(mmio.write(platform::ScspRuntime::kKeyOn, 2, 1U));
    mmio.scsp().onVBlank();
    platform::ScspRuntime::AudioFrame frame;
    REQUIRE(mmio.scsp().consumeAudio(frame));
    REQUIRE(frame.sampleRate == platform::ScspRuntime::kAudioSampleRate);
    REQUIRE(frame.channels == platform::ScspRuntime::kAudioChannels);
    REQUIRE(frame.samples.size() == (platform::ScspRuntime::kAudioSampleRate / 60U) * 2U);
    REQUIRE(std::any_of(frame.samples.begin(), frame.samples.end(),
                        [](float value) { return value != 0.0F; }));
    REQUIRE(mmio.scsp().stats().audioFrames == 1U);
    REQUIRE(mmio.scsp().stats().activeVoices == 1U);
}

void changesPcmParametersWithoutRestartingStream() {
    platform::ScspRuntime sound;
    sir::MachineState state;
    constexpr auto ram = platform::ScspRuntime::kRamBase;
    constexpr auto host = 0x900U;
    const auto put = [&](std::uint32_t offset, std::uint8_t width, std::uint32_t value) {
        REQUIRE(sound.write(ram + offset, width, value));
    };
    const auto get = [&](std::uint32_t offset) { return sound.read(ram + offset, 1U); };
    const auto submit = [&] {
        put(0x4E0U, 1U, 0x80U);
        sound.onSchedulerTick(state);
    };
    put(0x404U, 4U, host);
    for (std::uint32_t i = 0; i < 8U; ++i) put(0x10000U + i, 1U, i * 16U);
    put(host, 4U, 0x850010E0U); // mono 8-bit stream, maximum direct level
    put(host + 4U, 4U, 0x10000008U); // eight samples at 0x10000
    put(host + 8U, 4U, 0U);
    put(host + 12U, 4U, 0U);
    put(host + 12U, 1U, 16U); // start with half amplitude
    submit();
    REQUIRE(get(host) == 0U);
    sound.onVBlank(); // 735 samples leave the cursor at sample 7.
    platform::ScspRuntime::AudioFrame frame;
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[2] == 0.0625F);

    put(host, 4U, 0x8A0000E0U);
    put(host + 4U, 2U, 0x7800U); // half pitch
    put(host + 6U, 2U, 0U);      // no DSP routing
    put(host + 8U, 1U, 16U);    // half amplitude
    submit();
    REQUIRE(get(host) == 0U);
    sound.onVBlank();
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[0] == 0.4375F);
    REQUIRE(frame.samples[2] == 0.4375F);
    REQUIRE(frame.samples[4] == 0.0F);
    REQUIRE(sound.stats().pcmStarts == 1U);

    put(host, 4U, 0x8A0000EFU); // maximum direct level, pan hard right
    put(host + 4U, 2U, 0U);
    put(host + 8U, 1U, 0U);
    submit();
    REQUIRE(get(host) == 0U);
    sound.onVBlank();
    REQUIRE(sound.consumeAudio(frame));
    REQUIRE(frame.samples[0] == 0.0F);
    REQUIRE(frame.samples[1] == 0.75F);

    put(host, 4U, 0x8A0000E0U);
    put(host + 6U, 2U, 0x0100U); // DSP input requested; not implemented yet.
    submit();
    REQUIRE(get(host) == 0x8AU);
}

void followsRealtimeAudioClockBetweenVblanks() {
    platform::MmioInterceptor mmio;
    auto& scsp = mmio.scsp();
    scsp.setRealtimeAudioClock(true);
    scsp.onVBlank();
    platform::ScspRuntime::AudioFrame frame;
    REQUIRE(scsp.consumeAudio(frame));
    REQUIRE(frame.samples.size() ==
            (platform::ScspRuntime::kAudioSampleRate / 60U) * 2U);

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    scsp.onVBlank();
    REQUIRE(scsp.consumeAudio(frame));
    REQUIRE(frame.samples.size() >
            (platform::ScspRuntime::kAudioSampleRate / 60U) * 2U);
    REQUIRE(scsp.stats().audioSamples >
            2U * (platform::ScspRuntime::kAudioSampleRate / 60U));
}

void injectsScuVblankThroughRte() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address handlerAddress = 0x200U;
    constexpr core::Address vbr = 0x400U;
    constexpr core::Address stack = 0x800U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "main");
    auto& handler = module.createBlock(handlerAddress, "vblank");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);
    builder.setInsertPoint(handler);
    builder.returnFromException();

    platform::MmioInterceptor mmio;
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4, 0U));
    sir::MachineState state;
    state.setMemoryInterceptor(&mmio);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), vbr);
    state.writeState(sh2::stateSlot(sh2::Register::R15), stack);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(vbr + 0x40U * 4U, handlerAddress);

    platform::ScuEventScheduler masked(1U);
    // Generate a factor while it is masked, then verify it remains pending
    // until the SCU mask is opened again.
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4,
                       0xFFFFFFFFU));
    REQUIRE(!masked.beforeBlock(entry, state, 0U, mmio));
    REQUIRE(masked.stats().vblankEvents == 1U);
    REQUIRE(masked.stats().maskedEvents == 1U);
    REQUIRE((mmio.scuInterruptStatus() &
             platform::ScuEventScheduler::kVBlankInStatus) != 0U);
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4, 0U));
    const auto delivered = masked.beforeBlock(entry, state, 2U, mmio);
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->target == handlerAddress);
    REQUIRE(delivered->returnMode == sir::InterruptReturnMode::Exception);

    platform::ScuEventScheduler scheduler(2U);
    sir::ProgramExecutionOptions options;
    options.maxSteps = 6U;
    options.resolveBlock = [&module](core::Address address) {
        return module.blockAt(address);
    };
    options.interrupt = [&scheduler, &mmio](const sir::BasicBlock& block,
                                            sir::MachineState& current,
                                            std::size_t step) {
        return scheduler.beforeBlock(block, current, step, mmio);
    };
    const auto result = sir::ProgramExecutor{}.run(&entry, state, options);
    REQUIRE(result.status == sir::ProgramStatus::StepLimit);
    REQUIRE(scheduler.stats().vblankEvents == 3U);
    REQUIRE(scheduler.stats().deliveredInterrupts == 3U);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R15)) == stack);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::SR)) == 0U);
    REQUIRE(state.read32BE(stack - 8U) == entryAddress);
    REQUIRE(state.read32BE(stack - 4U) == 0U);
}

void returnsFromScuFirmwareCallbackThroughExceptionBridge() {
    constexpr core::Address entryAddress = 0x100U;
    constexpr core::Address callbackAddress = 0x200U;
    constexpr core::Address stack = 0x800U;
    constexpr std::uint32_t originalPr = 0x06001234U;

    sir::Module module;
    auto& entry = module.createBlock(entryAddress, "callback-main");
    auto& callback = module.createBlock(callbackAddress, "scu-callback");
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.branch(&entry);
    builder.setInsertPoint(callback);
    builder.writeState(sh2::stateSlot(sh2::Register::R4), builder.constI32(0xDEADBEEF));
    builder.ret(builder.readState(sir::kProcedureRegisterStateSlot));

    platform::MmioInterceptor mmio;
    REQUIRE(mmio.write(platform::BootRomServices::kScuInterruptMaskRegister, 4, 0U));
    sir::MachineState state;
    state.setMemoryInterceptor(&mmio);
    state.writeState(sh2::stateSlot(sh2::Register::R15), stack);
    state.writeState(sh2::stateSlot(sh2::Register::R4), 0x12345678U);
    state.writeState(sh2::stateSlot(sh2::Register::PR), originalPr);
    state.writeState(sh2::stateSlot(sh2::Register::SR), 0U);
    state.write32BE(platform::ScuEventScheduler::kScuInterruptTable +
                        platform::ScuEventScheduler::kVBlankInVector * 4U,
                    callbackAddress);

    platform::ScuEventScheduler scheduler;
    scheduler.requestVBlank();
    sir::ProgramExecutionOptions options;
    options.maxSteps = 3U;
    options.resolveBlock = [&module](core::Address address) {
        return module.blockAt(address);
    };
    options.interrupt = [&scheduler, &mmio](const sir::BasicBlock& block,
                                            sir::MachineState& current,
                                            std::size_t step) {
        return scheduler.beforeBlock(block, current, step, mmio);
    };
    const auto result = sir::ProgramExecutor{}.run(&entry, state, options);
    REQUIRE(result.status == sir::ProgramStatus::StepLimit);
    REQUIRE(scheduler.stats().deliveredInterrupts == 1U);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R15)) == stack);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::PR)) == originalPr);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::R4)) == 0x12345678U);
    REQUIRE(state.readState(sh2::stateSlot(sh2::Register::SR)) == 0U);
}

} // namespace

void registerSirTests(test::Registry& registry) {
    registry.add("sir: caches static dispatch until block body changes",
                 cachesStaticDispatchUntilBlockBodyChanges);
    registry.add("saturn: fills CD buffer while host drains FIFO",
                 producesCdDataWhileHostDrainsFifo);
    registry.add("saturn: delivers 150 CD data sectors per 75 subcode reports",
                 producesDoubleSpeedCdDataAtSubcodeCadence);
    registry.add("saturn: completes CD seek without stale PLAY sectors",
                 completesCdSeekWithoutProducingStalePlaySectors);
    registry.add("saturn: preserves CD command packet across subcode tick",
                 preservesCdCommandPacketAcrossSubcodeTick);
    registry.add("saturn: waits for complete CD calculation range",
                 waitsForCompleteCdCalculationRange);
    registry.add("sir: concrete module and interpreter", storesAndInterpretsConcreteSir);
    registry.add("sir: Win64 JIT matches interpreter for pure blocks",
                 win64JitMatchesInterpreterForPureSir);
    registry.add("sir: Win64 JIT matches interpreter for RAM", win64JitMatchesInterpreterForRam);
    registry.add("sir: Win64 JIT matches interpreter for MMIO", win64JitMatchesInterpreterForMmio);
    registry.add("sir: Win64 JIT preserves live state around memory", win64JitPreservesLiveStateAroundMemory);
    registry.add("sir: Win64 JIT propagates memory exceptions", win64JitPropagatesMemoryExceptions);
    registry.add("sir: Win64 JIT falls back before memory effects", win64JitFallsBackBeforeMemoryEffects);
    registry.add("differential: delayed conditional block", comparesDelayedConditionalBlock);
    registry.add("differential: memory and return block", comparesMemoryAndReturnBlock);
    registry.add("differential: carry shift and division", comparesCarryShiftAndDivision);
    registry.add("differential: string compare", comparesStringCompare);
    registry.add("sir: multi-block step limit", runsMultipleBlocksWithStepLimit);
    registry.add("sir: observes block entries", observesBlockEntries);
    registry.add("saturn: installs boot service table", installsBootServiceTable);
    registry.add("saturn: executes BUP init semantics", executesBootBupInitSemantics);
    registry.add("saturn: continues after SYS_EXECDMP when enabled",
                 continuesAfterExecuteDmpWhenEnabled);
    registry.add("saturn: traces boot service calls", tracesBootServiceCallerAndRegisters);
    registry.add("saturn: executes boot dispatch semantics", executesBootDispatchSemantics);
    registry.add("saturn: executes SMPC INTBACK", executesSmpcIntbackWithConfiguredPad);
    registry.add("saturn: refreshes SMPC INTBACK at VBlank", refreshesSmpcIntbackAtVblank);
    registry.add("saturn: executes CD image-backed FIFO", executesCdImageBackedFifo);
    registry.add("saturn: transfers CD FIFO through SCU DMA", transfersCdFifoThroughScuDma);
    registry.add("saturn: triggers SCU DMA at VBlank", triggersScuDmaAtVblank);
    registry.add("saturn: dispatches SCU DMA completion interrupt",
                 dispatchesScuDmaCompletionInterrupt);
    registry.add("saturn: dispatches SCU Timer 0 interrupt",
                 dispatchesScuTimer0AtConfiguredScanline);
    registry.add("saturn: dispatches VDP1 draw-end interrupt",
                 dispatchesVdp1DrawEndInterrupt);
    registry.add("saturn: dispatches SCU HBlank before first VDP1 list",
                 dispatchesScuHblankBeforeFirstVdp1List);
    registry.add("saturn: dispatches SCU VBlank-out after vertical interval",
                 dispatchesScuVblankOutAfterVerticalInterval);
    registry.add("saturn: reads ISO resource from CD image", readsIsoResourceFromCdImage);
    registry.add("saturn: completes CD sector length and publishes subcode Q",
                 completesCdSectorLengthAndPublishesSubcodeQ);
    registry.add("saturn: queries CD hardware, subcode, and filters",
                 queriesCdBlockHardwareSubcodeAndFilters);
    registry.add("saturn: schedules CD subcode independently from VBlank",
                 schedulesCdSubcodeIndependentlyFromVblank);
    registry.add("saturn: preserves CD cadence across CPU budgets",
                 keepsCdCadenceWhenChangingCpuBudget);
    registry.add("saturn: streams CD PLAY past retained-buffer capacity",
                 streamsCdPlayPastRetainedBufferCapacity);
    registry.add("saturn: DMA loads ISO resource into VDP2", dmaLoadsIsoResourceIntoVdp2);
    registry.add("saturn: commits VDP1 framebuffer", commitsVdp1FramebufferAsFrame);
    registry.add("saturn: composes VDP2 layers behind VDP1", composesVdp2LayersBehindVdp1);
    registry.add("saturn: compares VDP1 sprite and VDP2 priorities per pixel",
                 comparesVdp1SpritePriorityPerPixelWithVdp2Layer);
    registry.add("saturn: calculates VDP2 color across top and second layers",
                 calculatesVdp2ColorBetweenTopAndSecondLayers);
    registry.add("saturn: calculates sprite color and VDP2 shadows",
                 calculatesVdp1SpriteColorAndVdp2Shadows);
    registry.add("saturn: publishes VDP2 BACK and honors explicit priority",
                 publishesVdp2BackAndHonorsExplicitPriority);
    registry.add("saturn: resolves VDP2 equal priorities in hardware order",
                 resolvesVdp2LayerTiesInHardwareOrder);
    registry.add("saturn: captures VDP2 priority state",
                 capturesVdp2PriorityStateWithDiagnosticMemory);
    registry.add("saturn: decodes VDP1 character size fields",
                 decodesVdp1CharacterSizeXAndYFields);
    registry.add("saturn: decodes VDP2 24-bit RGB bitmap pixels",
                 composesVdp2Rgb888BitmapWithoutAlphaBit);
    registry.add("saturn: composes VDP2 continuous 320x224 bitmap",
                 composesVdp2Continuous320BitmapWithLineAndCellScroll);
    registry.add("saturn: excludes conflicting VDP2 high-color layers",
                 excludesVdp2LayersWhoseFetchSlotsAreUsedByHighColorModes);
    registry.add("saturn: adds VDP2 relative scroll base",
                 composesVdp2ContinuousBitmapAddsRelativeScrollBase);
    registry.add("saturn: composes VDP2 interleaved cell scroll",
                 composesVdp2ContinuousBitmapWithInterleavedCellScroll);
    registry.add("saturn: composes VDP2 cell pattern name and tile", composesVdp2CellPatternNameAndTile);
    registry.add("saturn: addresses VDP2 pattern name planes", addressesVdp2PatternNamePlanes);
    registry.add("saturn: composes pages within multi-page VDP2 planes",
                 composesVdp2PagesWithinMultiPagePlanes);
    registry.add("saturn: applies VDP2 rectangular window to NBG0",
                 appliesVdp2RectangularWindowToNbg0);
    registry.add("saturn: applies VDP2 inverted window to NBG0",
                 appliesVdp2InvertedWindowToNbg0);
    registry.add("saturn: interprets VDP1 command table", interpretsVdp1CommandTable);
    registry.add("saturn: executes automatic VDP1 command table at frame boundary",
                 executesAutomaticVdp1CommandTableAtFrameBoundary);
    registry.add("saturn: renders VDP1 textured sprite with CLUT and clipping",
                 rendersVdp1TexturedSpriteWithClutAndClipping);
    registry.add("saturn: Vulkan VDP1 covers 224, 240 and 256 line displays",
                 rendersVulkanVdp1AcrossFullDisplayHeight);
    registry.add("saturn: renders VDP1 eight-bit bank sprite from CRAM",
                 rendersVdp1EightBitBankSpriteFromCram);
    registry.add("sir: resolves indirect target on demand", resolvesIndirectTargetOnDemand);
    registry.add("sir: executes code loaded after boot image discovery",
                 executesCodeLoadedAfterBootImageDiscovery);
    registry.add("sir: timestamps runtime overlay replacement",
                 recordsRuntimeOverlayReplacementAtResolutionStep);
    registry.add("sir: intercepts Saturn MMIO", interceptsSaturnMmioWithoutTouchingRam);
    registry.add("sir: bounds retained MMIO access history",
                 boundsRetainedMmioAccessHistory);
    registry.add("saturn: executes SH-2 division unit window",
                 executesSh2DivisionUnitWindow);
    registry.add("saturn: advances SH-2 free-running timer",
                 advancesSh2FreeRunningTimer);
    registry.add("saturn: synchronizes SH-2 timer with display budget",
                 synchronizesSh2TimerWithDisplayBudget);
    registry.add("saturn: executes SH-2 word multiply", executesSh2WordMultiply);
    registry.add("saturn: handles cache-bypassing peripheral aliases",
                 handlesCacheBypassingPeripheralAliases);
    registry.add("saturn: maps SCSP and services secondary mailbox",
                 mapsScspWindowAndServicesMailbox);
    registry.add("saturn: consumes PCM commands and publishes playback position",
                 consumesPcmCommandsAndPublishesPlaybackPosition);
    registry.add("saturn: changes PCM parameters without restarting playback",
                 changesPcmParametersWithoutRestartingStream);
    registry.add("saturn: renders SCSP audio frame at VBlank",
                 rendersScspAudioFrameAtVblank);
    registry.add("saturn: follows real-time audio clock between VBlanks",
                 followsRealtimeAudioClockBetweenVblanks);
    registry.add("saturn: presents video only at scheduler frame boundary",
                 presentsOnlyAtSchedulerFrameBoundary);
    registry.add("saturn: captures VDP1 and VDP2 presentation diagnostics",
                 capturesVideoDiagnosticSnapshotAtPresentationBoundary);
    registry.add("saturn: captures first reenabled black cell frame", capturesReenabledBlackCellFrame);
    registry.add("saturn: swaps VDP1 manual framebuffer at presentation",
                 swapsVdp1ManualFramebufferAtPresentation);
    registry.add("saturn: injects SCU VBlank through RTE", injectsScuVblankThroughRte);
    registry.add("saturn: returns from SCU firmware callback bridge",
                 returnsFromScuFirmwareCallbackThroughExceptionBridge);
}
