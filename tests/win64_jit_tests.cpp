#include "test_support.hpp"

#include "saturn/sh2/cfg.hpp"
#include "saturn/sh2/module_lifter.hpp"
#include "saturn/saturn/mmio.hpp"
#include "saturn/sir/win64_jit.hpp"

#include <algorithm>
#include <array>

namespace {
using namespace saturn;

sir::ProgramExecutionOptions nativeOptions(sir::Win64JitBlockExecutor& jit) {
    sir::ProgramExecutionOptions options;
    options.executeBlock = [&jit](const sir::BasicBlock& block, sir::MachineState& state) {
        return jit.execute(block, state);
    };
    options.executeChain = [&jit](const sir::BasicBlock& block, sir::MachineState& state,
                                  std::size_t budget, const sir::BlockChainContinuation& continuation) {
        return jit.executeChain(block, state, budget, continuation);
    };
    return options;
}

void preservesPagedWordAccess() {
    sir::MachineState state;
    for (const core::Address address : {0x1000U, 0xFFFCU, 0xFFFDU, 0xFFFEU, 0xFFFFU, 0xFFFFFFFEU}) {
        REQUIRE(state.read32BE(address) == 0U);
        state.write32BE(address, 0x89ABCDEFU);
        REQUIRE(state.read32BE(address) == 0x89ABCDEFU);
        REQUIRE(state.read8(address) == 0x89U);
        REQUIRE(state.read16BE(address + 1U) == 0xABCDU);
        auto copied = state;
        REQUIRE(copied == state);
        state.write16BE(address + 1U, 0x1234U);
        REQUIRE(state.read32BE(address) == 0x891234EFU);
        REQUIRE(state.memory().at(address + 1U) == 0x12U);
        REQUIRE(copied.read32BE(address) == 0x89ABCDEFU);
        state.write32BE(address, 0U);
        REQUIRE(state.read32BE(address) == 0U);
        REQUIRE(state.memory().empty());
    }
}

void preservesDenseStateVisibility() {
    sir::MachineState state;
    state.writeState(100U, 99U); // Sparse extension remains independent.
    auto* slots = state.nativeStateData();
    slots[1U] = 123U;
    REQUIRE(state.stateSlots().at(1U) == 123U);
    slots[1U] = 456U; // A second snapshot must see writes through the old pointer.
    REQUIRE(state.stateSlots().at(1U) == 456U);
    slots[1U] = 0U;
    REQUIRE(!state.stateSlots().contains(1U));
    REQUIRE(state.stateSlots().at(100U) == 99U);
    auto copied = state;
    copied.nativeStateData()[1U] = 7U;
    REQUIRE(state.readState(1U) == 0U);
    REQUIRE(copied.readState(1U) == 7U);
    if (!sir::Win64JitBlockExecutor::available()) return;

    class Probe final : public sir::MemoryInterceptor {
    public:
        explicit Probe(sir::MachineState& state) : state_(state) {}
        void setExecutionContext(const sir::ExecutionContext&) override {}
        std::optional<std::uint32_t> read(core::Address, std::uint8_t) override {
            values.push_back(state_.stateSlots().contains(1U) ? state_.stateSlots().at(1U) : 0U);
            return 0U;
        }
        bool write(core::Address, std::uint8_t, std::uint32_t) override {
            static_cast<void>(read(0U, 0U));
            return true;
        }
        sir::MachineState& state_;
        std::vector<std::uint32_t> values;
    };
    sir::Module module;
    auto& block = module.createBlock(0x100U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(block);
    const auto address = builder.constI32(0x1000U);
    builder.writeState(1U, builder.constI32(123U));
    builder.store32(address, builder.readState(1U));
    builder.writeState(1U, builder.constI32(456U));
    builder.store32(address, builder.readState(1U));
    builder.writeState(1U, builder.constI32(0U));
    static_cast<void>(builder.load32(address));
    builder.trap(0x7FU);
    block.markLowered();
    sir::MachineState expected;
    sir::MachineState actual;
    Probe referenceProbe(expected);
    Probe nativeProbe(actual);
    expected.setMemoryInterceptor(&referenceProbe);
    actual.setMemoryInterceptor(&nativeProbe);
    const auto reference = sir::Interpreter{}.execute(block, expected);
    sir::Win64JitBlockExecutor jit;
    const auto native = jit.execute(block, actual);
    REQUIRE(native.has_value());
    REQUIRE(native->status == reference.status);
    REQUIRE(actual == expected);
    REQUIRE(nativeProbe.values == std::vector<std::uint32_t>({123U, 456U, 0U}));
    REQUIRE(nativeProbe.values == referenceProbe.values);
}

void flaggedArithmeticAndDivision() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& pure = module.createBlock(0x100U);
    auto& memory = module.createBlock(0x200U);
    sir::ModuleBuilder builder(module);
    for (auto* block : {&pure, &memory}) {
        builder.setInsertPoint(*block);
        const auto left = builder.readState(0U);
        const auto right = builder.readState(1U);
        const auto carry = builder.readState(2U);
        const std::array flagged = {
            builder.addWithCarryI32(left, right, carry),
            builder.subWithBorrowI32(left, right, carry),
            builder.addWithOverflowI32(left, right),
            builder.subWithOverflowI32(left, right),
        };
        for (std::size_t index = 0; index < flagged.size(); ++index) {
            builder.writeState(static_cast<sir::StateSlot>(100U + index * 2U), flagged[index].value);
            builder.writeState(static_cast<sir::StateSlot>(101U + index * 2U), flagged[index].flag);
        }
        auto value = left;
        auto q = builder.readState(3U);
        const auto m = builder.readState(4U);
        auto t = builder.readState(5U);
        for (unsigned step = 0; step < 32U; ++step) {
            const auto divided = builder.divideStepI32(value, right, q, m, t);
            builder.writeState(static_cast<sir::StateSlot>(200U + step * 3U), divided.value);
            builder.writeState(static_cast<sir::StateSlot>(201U + step * 3U), divided.q);
            builder.writeState(static_cast<sir::StateSlot>(202U + step * 3U), divided.t);
            value = divided.value;
            q = divided.q;
            t = divided.t;
        }
        if (block == &memory) {
            const auto address = builder.constI32(0x1000);
            builder.store32(address, value);
            builder.writeState(6U, builder.load32(address));
        }
        builder.ret(value);
        block->markLowered();
    }
    sir::Win64JitBlockExecutor jit;
    const auto compare = [&](std::uint32_t left, std::uint32_t right,
                              std::uint32_t carry, std::uint32_t flags) {
        sir::MachineState initial;
        initial.writeState(0U, left);
        initial.writeState(1U, right);
        initial.writeState(2U, carry);
        initial.writeState(3U, flags & 1U ? 0x80000000U : 0U);
        initial.writeState(4U, flags & 2U ? 2U : 0U);
        initial.writeState(5U, flags & 4U ? 7U : 0U);
        for (auto* block : {&pure, &memory}) {
            auto expected = initial;
            const auto interpreted = sir::Interpreter{}.execute(*block, expected);
            auto actual = initial;
            const auto native = jit.execute(*block, actual);
            REQUIRE(native.has_value());
            REQUIRE(native->status == interpreted.status);
            REQUIRE(native->control == interpreted.control);
            REQUIRE(native->dynamicTarget == interpreted.dynamicTarget);
            REQUIRE(actual == expected);
        }
    };
    constexpr std::array edges = {0U, 1U, 2U, 0x7FFFFFFEU, 0x7FFFFFFFU,
                                  0x80000000U, 0x80000001U, 0xFFFFFFFEU, 0xFFFFFFFFU};
    for (const auto left : edges) {
        for (const auto right : edges) {
            for (std::uint32_t flags = 0; flags < 8U; ++flags) {
                for (const auto carry : {0U, 1U, 2U, 0xFFFFFFFFU}) compare(left, right, carry, flags);
            }
        }
    }
    std::uint32_t seed = 0xC0FFEEU;
    const auto random = [&]() {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        return seed;
    };
    for (unsigned trial = 0; trial < 1024U; ++trial) {
        const auto left = random();
        const auto right = random();
        const auto carry = random();
        const auto flags = random();
        compare(left, right, carry, flags);
    }
    REQUIRE(jit.compiledBlockCount() == 2U);
    REQUIRE(jit.executedBlockCount() == 7232U);

    // MOV #-1,R1; MOV #1,R2; ADDC R2,R1; SHLL R1; DIV0U;
    // DIV1 R2,R1; RTS; NOP. Exercise the actual SH-2 lowering path.
    const auto image = test::imageFromWords(0x400U, {
        0xE1FFU, 0xE201U, 0x312EU, 0x4100U, 0x0019U, 0x3124U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x400U});
    sir::Module lowered;
    REQUIRE(sh2::lowerCFG(cfg, lowered).success());
    sir::MachineState actual, expected;
    const auto interpreted = sir::Interpreter{}.execute(*lowered.blockAt(0x400U), expected);
    const auto native = jit.execute(*lowered.blockAt(0x400U), actual);
    REQUIRE(native.has_value());
    REQUIRE(native->control == interpreted.control);
    REQUIRE(native->dynamicTarget == interpreted.dynamicTarget);
    REQUIRE(actual == expected);
}

void chainsDirectBlocks() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    auto& loop = module.createBlock(0x104U);
    auto& call = module.createBlock(0x108U);
    auto& returned = module.createBlock(0x114U);
    auto& callee = module.createBlock(0x200U);
    auto& function = module.createFunction(0x200U);
    function.setEntry(&callee);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.writeState(0U, builder.constI32(0));
    builder.branch(&loop);
    builder.setInsertPoint(loop);
    const auto count = builder.addI32(builder.readState(0U), builder.constI32(1));
    builder.writeState(0U, count);
    builder.writeState(3U, builder.readState(sir::kProgramCounterStateSlot));
    builder.branchIf(builder.cmpUnsignedGreaterEqualI32(count, builder.readState(2U)), &call, &loop);
    builder.setInsertPoint(call);
    builder.writeState(sir::kProcedureRegisterStateSlot, builder.constI32(0x114));
    builder.call(&function, &returned);
    builder.setInsertPoint(callee);
    const auto address = builder.constI32(0x1000);
    builder.store32(address, builder.readState(0U));
    builder.writeState(4U, builder.load32(address));
    builder.ret(builder.readState(sir::kProcedureRegisterStateSlot));
    builder.setInsertPoint(returned);
    builder.trap(0x55U);
    for (auto* block : {&entry, &loop, &call, &returned, &callee}) block->markLowered();
    sir::Win64JitBlockExecutor jit;
    std::size_t largestChain{};
    sir::ProgramExecutionOptions options;
    options.resolveBlock = [&module](core::Address target) { return module.blockAt(target); };
    auto native = nativeOptions(jit);
    native.resolveBlock = options.resolveBlock;
    native.executeChain = [&jit, &largestChain](const sir::BasicBlock& block,
                                               sir::MachineState& state, std::size_t budget,
                                               const sir::BlockChainContinuation& continuation) {
        auto result = jit.executeChain(block, state, budget, continuation);
        if (result) largestChain = std::max(largestChain, result->blocksExecuted);
        return result;
    };
    for (const auto budget : {0U, 1U, 2U, 10U, 14U, 15U}) {
        sir::MachineState actual, expected;
        actual.writeState(2U, 10U);
        expected = actual;
        options.maxSteps = native.maxSteps = budget;
        const auto reference = sir::ProgramExecutor{}.run(&entry, expected, options);
        const auto executedBefore = jit.executedBlockCount();
        const auto result = sir::ProgramExecutor{}.run(&entry, actual, native);
        REQUIRE(jit.executedBlockCount() - executedBefore == result.steps);
        REQUIRE(result.status == reference.status);
        REQUIRE(result.steps == reference.steps);
        REQUIRE(result.address == reference.address);
        REQUIRE(result.unresolvedTarget == reference.unresolvedTarget);
        REQUIRE(result.last.control == reference.last.control);
        REQUIRE(result.last.dynamicTarget == reference.last.dynamicTarget);
        REQUIRE(result.last.trapVector == reference.last.trapVector);
        REQUIRE(result.last.target == reference.last.target);
        REQUIRE(actual == expected);
    }
    REQUIRE(largestChain == 14U);
    // Tail jumps must not grow the host stack during a long native loop.
    sir::MachineState actual, expected;
    actual.writeState(2U, 0xFFFFFFFFU);
    expected = actual;
    options.maxSteps = native.maxSteps = 200000U;
    const auto reference = sir::ProgramExecutor{}.run(&loop, expected, options);
    const auto result = sir::ProgramExecutor{}.run(&loop, actual, native);
    REQUIRE(result.status == sir::ProgramStatus::StepLimit);
    REQUIRE(result.steps == reference.steps);
    REQUIRE(result.address == reference.address);
    REQUIRE(actual == expected);
    REQUIRE(largestChain == 200000U);
}

void preservesBoundariesAndFallback() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    auto& unfinished = module.createBlock(0x104U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.writeState(1U, builder.constI32(7));
    builder.branch(&unfinished);
    entry.markLowered();
    builder.setInsertPoint(unfinished);
    builder.writeState(2U, builder.readState(sir::kProgramCounterStateSlot));
    builder.trap(1U);
    sir::Win64JitBlockExecutor jit;
    sir::MachineState state;
    const auto chain = jit.executeChain(entry, state, 10U);
    REQUIRE(chain.has_value());
    REQUIRE(chain->blocksExecuted == 1U);
    REQUIRE(chain->lastBlock == &entry);
    REQUIRE(chain->last.target == &unfinished);
    REQUIRE(state.readState(sir::kProgramCounterStateSlot) == entry.address());
    REQUIRE(state.readState(2U) == 0U);
    auto options = nativeOptions(jit);
    std::size_t chainCalls{};
    options.executeChain = [&jit, &chainCalls](const sir::BasicBlock& block,
                                               sir::MachineState& machine, std::size_t budget,
                                               const sir::BlockChainContinuation& continuation) {
        ++chainCalls;
        return jit.executeChain(block, machine, budget, continuation);
    };
    sir::MachineState expected, actual;
    const auto reference = sir::ProgramExecutor{}.run(&entry, expected);
    const auto native = sir::ProgramExecutor{}.run(&entry, actual, options);
    REQUIRE(native.status == reference.status);
    REQUIRE(native.steps == reference.steps);
    REQUIRE(actual == expected);
    REQUIRE(chainCalls == 2U);
    for (unsigned kind = 0; kind < 4U; ++kind) {
        auto hooked = options;
        std::size_t boundaries{};
        if (kind == 0U) hooked.beforeBlock = [&](const sir::BasicBlock&, sir::MachineState&, std::size_t) {
            ++boundaries;
        };
        if (kind == 1U) hooked.observeBlock = [&](const sir::BasicBlock&, std::size_t) { ++boundaries; };
        if (kind == 2U) hooked.interrupt = [&](const sir::BasicBlock&, sir::MachineState&, std::size_t)
            -> std::optional<sir::InterruptRequest> { ++boundaries; return std::nullopt; };
        platform::MmioInterceptor mmio;
        sir::MachineState withHooks;
        if (kind == 3U) withHooks.setMemoryInterceptor(&mmio);
        const auto callsBefore = chainCalls;
        const auto result = sir::ProgramExecutor{}.run(&entry, withHooks, hooked);
        REQUIRE(result.status == reference.status);
        REQUIRE(result.steps == reference.steps);
        REQUIRE(withHooks == expected);
        REQUIRE(chainCalls == callsBefore + 2U);
        if (kind != 3U) REQUIRE(boundaries == 2U);
    }
    platform::MmioInterceptor mmio;
    state.setMemoryInterceptor(&mmio);
    REQUIRE(!jit.executeChain(entry, state, 10U).has_value());
    REQUIRE(!jit.executeChain(entry, state, 0U).has_value());
}
void chainsMmioAndNestedInterrupts() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    auto& loop = module.createBlock(0x104U);
    auto& done = module.createBlock(0x110U);
    auto& handler = module.createBlock(0x200U);
    auto& handlerReturn = module.createBlock(0x204U);
    auto& callback = module.createBlock(0x300U);
    auto& nested = module.createBlock(0x400U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.writeState(0U, builder.constI32(0));
    builder.branch(&loop);
    builder.setInsertPoint(loop);
    const auto count = builder.addI32(builder.readState(0U), builder.constI32(1));
    builder.writeState(0U, count);
    builder.store8(builder.constI32(0x05E00000), count);
    builder.writeState(7U, builder.load8(builder.constI32(0x05E00000)));
    builder.branchIf(builder.cmpEqI32(count, builder.constI32(3)), &done, &loop);
    builder.setInsertPoint(done);
    builder.trap(0x77U);
    builder.setInsertPoint(handler);
    builder.writeState(5U, builder.addI32(builder.readState(5U), builder.constI32(3)));
    builder.store16(builder.constI32(0x05E00002), builder.readState(5U));
    builder.branch(&handlerReturn);
    builder.setInsertPoint(handlerReturn);
    builder.returnFromException();
    builder.setInsertPoint(callback);
    builder.writeState(1U, builder.constI32(0x333));
    builder.store8(builder.constI32(0x05E00004), builder.readState(1U));
    builder.ret(builder.readState(sir::kProcedureRegisterStateSlot));
    builder.setInsertPoint(nested);
    builder.writeState(6U, builder.constI32(0x888));
    builder.store32(builder.constI32(0x05E00008), builder.readState(6U));
    builder.returnFromException();
    for (auto* block : {&entry, &loop, &done, &handler, &handlerReturn, &callback, &nested}) {
        block->markLowered();
    }
    sir::MachineState initial;
    initial.writeState(1U, 11U);
    initial.writeState(5U, 5U);
    initial.writeState(6U, 66U);
    initial.writeState(sir::kStackPointerStateSlot, 0x2000U);
    initial.writeState(sir::kProcedureRegisterStateSlot, 0x9000U);
    initial.writeState(sir::kStatusRegisterStateSlot, 0x13U);
    auto expected = initial;
    auto actual = initial;
    platform::MmioInterceptor referenceMmio, nativeMmio;
    expected.setMemoryInterceptor(&referenceMmio);
    actual.setMemoryInterceptor(&nativeMmio);
    std::vector<std::array<std::uint32_t, 3>> referenceBoundaries, nativeBoundaries;
    const auto configure = [&](sir::MachineState& state,
                               std::vector<std::array<std::uint32_t, 3>>& boundaries) {
        sir::ProgramExecutionOptions options;
        options.maxSteps = 100U;
        options.resolveBlock = [&module](core::Address address) { return module.blockAt(address); };
        options.interrupt = [fired = std::array<bool, 3>{}](const sir::BasicBlock& block,
                                                            sir::MachineState&, std::size_t) mutable
            -> std::optional<sir::InterruptRequest> {
            if (block.address() == 0x104U && !fired[0]) {
                fired[0] = true;
                return sir::InterruptRequest{0x200U, sir::InterruptReturnMode::Exception};
            }
            if (block.address() == 0x200U && !fired[1]) {
                fired[1] = true;
                return sir::InterruptRequest{0x300U, sir::InterruptReturnMode::Callback};
            }
            if (block.address() == 0x300U && !fired[2]) {
                fired[2] = true;
                return sir::InterruptRequest{0x400U, sir::InterruptReturnMode::Exception};
            }
            return std::nullopt;
        };
        options.beforeBlock = [](const sir::BasicBlock& block, sir::MachineState& state, std::size_t) {
            if (block.address() == 0x104U) state.writeState(10U, state.readState(10U) + 1U);
        };
        options.observeBlock = [&state, &boundaries](const sir::BasicBlock& block, std::size_t step) {
            boundaries.push_back({block.address(), static_cast<std::uint32_t>(step),
                                   state.readState(sir::kProgramCounterStateSlot)});
        };
        return options;
    };
    auto expectedOptions = configure(expected, referenceBoundaries);
    auto options = configure(actual, nativeBoundaries);
    sir::Win64JitBlockExecutor jit;
    const auto backend = nativeOptions(jit);
    options.executeBlock = backend.executeBlock;
    options.executeChain = backend.executeChain;
    const auto reference = sir::ProgramExecutor{}.run(&entry, expected, expectedOptions);
    const auto result = sir::ProgramExecutor{}.run(&entry, actual, options);
    REQUIRE(result.status == sir::ProgramStatus::Trap);
    REQUIRE(result.status == reference.status);
    REQUIRE(result.steps == reference.steps);
    REQUIRE(result.steps == 9U);
    REQUIRE(result.address == reference.address);
    REQUIRE(result.last.trapVector == reference.last.trapVector);
    REQUIRE(actual == expected);
    REQUIRE(nativeBoundaries == referenceBoundaries);
    REQUIRE(actual.readState(1U) == 11U);
    REQUIRE(actual.readState(5U) == 8U);
    REQUIRE(actual.readState(6U) == 66U);
    REQUIRE(actual.readState(10U) == 3U);
    REQUIRE(actual.readState(sir::kStackPointerStateSlot) == 0x2000U);
    REQUIRE(jit.executedBlockCount() == result.steps);
    REQUIRE(jit.chainedTransferCount() == result.steps - 1U);
    REQUIRE(nativeMmio.accesses().size() == referenceMmio.accesses().size());
    REQUIRE(nativeMmio.accesses().size() == 9U);
    for (std::size_t index = 0; index < nativeMmio.accesses().size(); ++index) {
        const auto& native = nativeMmio.accesses()[index];
        const auto& referenceAccess = referenceMmio.accesses()[index];
        REQUIRE(native.address == referenceAccess.address);
        REQUIRE(native.width == referenceAccess.width);
        REQUIRE(native.value == referenceAccess.value);
        REQUIRE(native.write == referenceAccess.write);
        REQUIRE(native.context.blockAddress == referenceAccess.context.blockAddress);
        REQUIRE(native.context.callerBlock == referenceAccess.context.callerBlock);
        REQUIRE(native.context.step == referenceAccess.context.step);
        REQUIRE(native.context.state == referenceAccess.context.state);
    }
}

void chainStopsOnBoundaryFailure() {
    if (!sir::Win64JitBlockExecutor::available()) return;
    sir::Module module;
    auto& entry = module.createBlock(0x100U);
    auto& next = module.createBlock(0x104U);
    sir::ModuleBuilder builder(module);
    builder.setInsertPoint(entry);
    builder.store8(builder.constI32(0x05E00000), builder.constI32(7));
    builder.branch(&next);
    builder.setInsertPoint(next);
    builder.writeState(1U, builder.constI32(99));
    builder.trap(1U);
    entry.markLowered();
    next.markLowered();
    for (const bool missingInterrupt : {false, true}) {
        sir::MachineState actual, expected;
        actual.writeState(sir::kStackPointerStateSlot, 0x2000U);
        expected = actual;
        platform::MmioInterceptor nativeMmio, referenceMmio;
        actual.setMemoryInterceptor(&nativeMmio);
        expected.setMemoryInterceptor(&referenceMmio);
        sir::ProgramExecutionOptions options;
        if (missingInterrupt) {
            options.interrupt = [](const sir::BasicBlock& block, sir::MachineState&, std::size_t)
                -> std::optional<sir::InterruptRequest> {
                if (block.address() == 0x104U) return sir::InterruptRequest{0x8000U};
                return std::nullopt;
            };
        } else {
            options.beforeBlock = [](const sir::BasicBlock& block, sir::MachineState& state, std::size_t) {
                if (block.address() == 0x104U) {
                    state.writeState(2U, 42U);
                    throw std::runtime_error("boundary failure");
                }
            };
        }
        auto native = options;
        sir::Win64JitBlockExecutor jit;
        const auto backend = nativeOptions(jit);
        native.executeBlock = backend.executeBlock;
        native.executeChain = backend.executeChain;
        if (missingInterrupt) {
            const auto reference = sir::ProgramExecutor{}.run(&entry, expected, options);
            const auto result = sir::ProgramExecutor{}.run(&entry, actual, native);
            REQUIRE(result.status == sir::ProgramStatus::UnresolvedTarget);
            REQUIRE(result.status == reference.status);
            REQUIRE(result.steps == reference.steps);
            REQUIRE(result.address == reference.address);
            REQUIRE(result.unresolvedTarget == reference.unresolvedTarget);
        } else {
            unsigned failures{};
            try { (void)sir::ProgramExecutor{}.run(&entry, expected, options); }
            catch (const std::runtime_error& error) {
                REQUIRE(std::string(error.what()) == "boundary failure");
                ++failures;
            }
            try { (void)sir::ProgramExecutor{}.run(&entry, actual, native); }
            catch (const std::runtime_error& error) {
                REQUIRE(std::string(error.what()) == "boundary failure");
                ++failures;
            }
            REQUIRE(failures == 2U);
        }
        REQUIRE(actual == expected);
        REQUIRE(actual.readState(1U) == 0U);
        REQUIRE(nativeMmio.accesses().size() == 1U);
        REQUIRE(referenceMmio.accesses().size() == 1U);
        REQUIRE(jit.executedBlockCount() == 1U);
        REQUIRE(jit.chainedTransferCount() == 0U);
    }
}
} // namespace

void registerWin64JitTests(test::Registry& registry) {
    registry.add("sir: word RAM accesses preserve page boundaries and snapshots",
                 preservesPagedWordAccess);
    registry.add("sir: dense native state remains visible to MMIO and snapshots",
                 preservesDenseStateVisibility);
    registry.add("sir: Win64 JIT flagged arithmetic and DIV1", flaggedArithmeticAndDivision);
    registry.add("sir: Win64 JIT chains direct blocks", chainsDirectBlocks);
    registry.add("sir: Win64 JIT chain preserves boundaries and fallback", preservesBoundariesAndFallback);
    registry.add("sir: Win64 JIT chains MMIO and nested interrupts", chainsMmioAndNestedInterrupts);
    registry.add("sir: Win64 JIT chain stops on boundary failure", chainStopsOnBoundaryFailure);
}
