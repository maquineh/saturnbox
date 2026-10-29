#include "saturn/saturn/slave.hpp"

namespace saturn::platform {

class SlaveSh2::Memory final : public sir::MemoryInterceptor {
public:
    MmioInterceptor onChip;
    sir::MemoryInterceptor* shared{};
    MmioInterceptor* master{};
    std::array<std::uint8_t, 512> registers{};
    bool waitingForCapture{};

    Memory() { onChip.setAccessRecording(false); }
    core::Address backingAddress(core::Address address) const noexcept override {
        return shared ? shared->backingAddress(address) : canonicalizeRamAddress(address);
    }
    bool requiresStateSnapshot(core::Address address) const noexcept override {
        return shared && shared->requiresStateSnapshot(address);
    }
    void setExecutionContext(const sir::ExecutionContext& context) override {
        onChip.setExecutionContext(context);
        if (shared) shared->setExecutionContext(context);
    }
    std::optional<std::uint32_t> read(core::Address address, std::uint8_t width) override {
        if (address < 0xFFFFFE00U) return shared ? shared->read(address, width) : std::nullopt;
        if (const auto value = onChip.read(address, width)) {
            if (address == 0xFFFFFE11U && width == 1U && (*value & 0x80U) == 0U) {
                waitingForCapture = true;
            }
            return value;
        }
        std::uint32_t value{};
        for (unsigned i = 0; i < width; ++i) {
            value = (value << 8U) | registers[(address + i) & 511U];
        }
        return value;
    }
    bool write(core::Address address, std::uint8_t width, std::uint32_t value) override {
        if ((address == 0x01800000U || address == 0x21800000U) && width == 2U) {
            master->captureFrtInput();
            return true;
        }
        if (address < 0xFFFFFE00U) return shared && shared->write(address, width, value);
        if (!onChip.write(address, width, value)) {
            for (unsigned i = 0; i < width; ++i) {
                registers[(address + i) & 511U] = static_cast<std::uint8_t>(
                    value >> ((width - i - 1U) * 8U));
            }
        }
        return true;
    }
};

SlaveSh2::SlaveSh2() = default;
SlaveSh2::~SlaveSh2() = default;

bool SlaveSh2::tick(sir::MachineState& state, BootRomServices& services,
                    MmioInterceptor& master, std::size_t step,
                    const sir::BlockResolver& resolve,
                    const sir::BlockExecutionHook& execute) {
    const sir::StepAwareBlockResolver stepAwareResolve =
        [&resolve](core::Address address, std::size_t) { return resolve(address); };
    return tick(state, services, master, step, stepAwareResolve, execute);
}

bool SlaveSh2::tick(sir::MachineState& state, BootRomServices& services,
                    MmioInterceptor& master, std::size_t step,
                    const sir::StepAwareBlockResolver& resolve,
                    const sir::BlockExecutionHook& execute) {
    if (hasStep_ && step == lastStep_) return !failed_;
    hasStep_ = true;
    lastStep_ = step;
    const auto capture = master.takeSlaveInputCapture();
    const auto generation = master.smpc().slaveResetGeneration();
    if (!master.smpc().slaveEnabled() || generation != resetGeneration_) {
        started_ = false;
        memory_.reset();
        resetGeneration_ = generation;
    }
    if (!master.smpc().slaveEnabled()) return true;
    if (!started_) {
        pc_ = services.slaveEntryPoint();
        if (pc_ == 0U) return true; // No application entry installed yet.
        registers_.fill(0U);
        registers_[sir::kStackPointerStateSlot] = 0x06001000U;
        registers_[sir::kVectorBaseStateSlot] = 0x06000400U;
        registers_[sir::kStatusRegisterStateSlot] = 0xF0U;
        previous_ = 0U;
        failed_ = false;
        memory_ = std::make_unique<Memory>();
        started_ = true;
    }
    if (failed_) return false;
    if (memory_->waitingForCapture && !capture) return true;
    if (capture) memory_->onChip.captureFrtInput();
    memory_->waitingForCapture = false;

    const auto masterContext = master.executionContext();
    auto* shared = state.memoryInterceptor();
    memory_->shared = shared;
    memory_->master = &master;
    std::array<std::uint32_t, sir::MachineState::kDenseStateSlots> saved{};
    for (sir::StateSlot slot = 0; slot < saved.size(); ++slot) {
        saved[slot] = state.readState(slot);
        state.writeState(slot, registers_[slot]);
    }
    state.setMemoryInterceptor(memory_.get());
    // Run a polling worker to its next idle read, rather than assigning one
    // slave basic block per master block. Block sizes differ between cores;
    // that ratio allowed DMA to see partially decoded textures. This is an
    // event-driven service boundary, not a cycle-accurate CPU/bus model.
    constexpr std::size_t kBurstLimit = 100'000U;
    sir::ExecutionContext context;
    for (std::size_t burst = 0; burst < kBurstLimit; ++burst) {
        const auto* block = resolve(pc_, step);
        if (!block || block->instructions().empty()) {
            failed_ = true;
            break;
        }
        state.writeState(sir::kProgramCounterStateSlot, pc_);
        context.blockAddress = pc_;
        context.callerBlock = previous_;
        context.step = step;
        context.state.clear();
        if (memory_->requiresStateSnapshot(pc_)) context.state = state.stateSlots();
        state.setExecutionContext(context);
        services.executeService(*block, state);
        const auto native = execute ? execute(*block, state) : std::nullopt;
        const auto result = native ? *native : sir::Interpreter{}.execute(*block, state);
        ++executedBlocks_;
        core::Address next{};
        if (result && !result.slept) {
            switch (result.control) {
            case sir::ControlKind::Branch:
                if (result.target) next = result.target->address();
                break;
            case sir::ControlKind::Call:
                if (result.function) next = result.function->address();
                break;
            case sir::ControlKind::IndirectBranch:
            case sir::ControlKind::IndirectCall:
            case sir::ControlKind::Return: next = result.dynamicTarget; break;
            default: break; // Exceptions require a separate interrupt model.
            }
        }
        previous_ = pc_;
        if (next == 0U) {
            failed_ = true;
            break;
        }
        pc_ = next;
        if (memory_->waitingForCapture) break;
    }
    for (sir::StateSlot slot = 0; slot < saved.size(); ++slot) {
        registers_[slot] = state.readState(slot);
        state.writeState(slot, saved[slot]);
    }
    state.setMemoryInterceptor(shared);
    state.setExecutionContext(masterContext);
    return !failed_;
}

} // namespace saturn::platform
