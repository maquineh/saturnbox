#pragma once

#include "saturn/saturn/boot_services.hpp"

#include <array>
#include <memory>

namespace saturn::platform {

// Cooperative execution of a firmware-started slave using FRT polling.
// An empty capture-flag read yields; MINIT resumes the worker to its next poll.
// Registers and on-chip devices are private; external RAM/devices are shared.
// Interrupt/exception delivery, service duration and cache timing are not modeled here.
class SlaveSh2 final {
public:
    SlaveSh2();
    ~SlaveSh2();
    SlaveSh2(const SlaveSh2&) = delete;
    SlaveSh2& operator=(const SlaveSh2&) = delete;

    [[nodiscard]] bool tick(sir::MachineState& state, BootRomServices& services,
                            MmioInterceptor& master, std::size_t step,
                            const sir::BlockResolver& resolve,
                            const sir::BlockExecutionHook& execute = {});
    [[nodiscard]] bool tick(sir::MachineState& state, BootRomServices& services,
                            MmioInterceptor& master, std::size_t step,
                            const sir::StepAwareBlockResolver& resolve,
                            const sir::BlockExecutionHook& execute = {});
    [[nodiscard]] std::size_t executedBlocks() const noexcept { return executedBlocks_; }
    [[nodiscard]] core::Address pc() const noexcept { return pc_; }

private:
    class Memory;
    std::unique_ptr<Memory> memory_;
    std::array<std::uint32_t, sir::MachineState::kDenseStateSlots> registers_{};
    core::Address pc_{}, previous_{};
    std::size_t executedBlocks_{}, lastStep_{};
    std::uint64_t resetGeneration_{};
    bool started_{}, hasStep_{}, failed_{};
};

} // namespace saturn::platform
