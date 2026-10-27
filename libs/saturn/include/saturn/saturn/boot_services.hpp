#pragma once

#include "saturn/core/address.hpp"
#include "saturn/saturn/mmio.hpp"
#include "saturn/sir/interpreter.hpp"
#include "saturn/sir/module.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace saturn::platform {

struct BootServiceSlot {
    core::Address pointerAddress{};
    core::Address targetAddress{};
    std::string_view name;
};

struct BootServiceCall {
    BootServiceSlot service;
    sir::ExecutionContext context;
};

// Minimal deterministic service table installed by the Saturn boot ROM before
// transferring control to an application's AIP. The table is intentionally
// explicit: service calls are represented by SIR blocks and never disappear
// into a generic "return on unknown address" rule.
class BootRomServices final : public sir::MemoryInterceptor {
public:
    static constexpr core::Address kCdInit2Pointer = 0x0600029CU;
    static constexpr core::Address kCdInit1Pointer = 0x060002DCU;
    static constexpr core::Address kExecuteDmpPointer = 0x0600026CU;
    static constexpr core::Address kAipDispatchPointer = 0x06000300U;
    static constexpr core::Address kAipGetDispatchPointer = 0x06000304U;
    static constexpr core::Address kAipSetSh2DispatchPointer = 0x06000310U;
    static constexpr core::Address kAipGetSh2DispatchPointer = 0x06000314U;
    static constexpr core::Address kAipChangeSystemClockPointer = 0x06000320U;
    static constexpr core::Address kAipTasSemaphorePointer = 0x06000330U;
    static constexpr core::Address kAipClearSemaphorePointer = 0x06000334U;
    static constexpr core::Address kAipSetScuMaskPointer = 0x06000340U;
    static constexpr core::Address kAipCdDispatchPointer = 0x06000344U;
    // The BIOS installs the backup-RAM entry point at this slot during boot.
    // Yabause exposes the same entry point as the BUPInit dispatcher.
    static constexpr core::Address kBupInitPointer = 0x06000358U;
    static constexpr core::Address kBupInitStubAddress = 0x0F000030U;
    // BUPInit returns a table of BIOS entry points at 0x380..0x3AC.  Keep
    // those addresses intact (rather than replacing them with host aliases)
    // so code that stores and calls the table remains Saturn-compatible.
    static constexpr core::Address kBupFunctionTableFirst = 0x00000380U;
    static constexpr core::Address kBupFunctionTableLast = 0x000003ACU;
    static constexpr core::Address kSystemClockStorage = 0x06000324U;
    static constexpr core::Address kScuInterruptTable = 0x06000900U;
    static constexpr core::Address kScuInterruptMaskStorage = 0x06000348U;
    static constexpr core::Address kSemaphoreBase = 0x06000B00U;
    static constexpr core::Address kScuInterruptMaskRegister = 0x25FE00A0U;
    static constexpr core::Address kScuInterruptStatusRegister = 0x25FE00A4U;
    static constexpr core::Address kAbusInterruptAcknowledge = 0x25FE00A8U;
    static constexpr core::Address kDummyInterruptHandler = 0x06000610U;
    static constexpr core::Address kAipSyncWord = 0x0600318CU;
    static constexpr core::Address kDefaultMasterStack = 0x06002000U;
    static constexpr core::Address kDefaultVectorBase = 0x06000400U;
    static constexpr core::Address kStubAddress = 0x0F000000U;
    static constexpr core::Address kExitStubAddress = 0x0F000004U;
    static constexpr core::Address kCdInit1StubAddress = 0x0F000008U;
    static constexpr core::Address kAipDispatchStubAddress = 0x0F00000CU;
    static constexpr core::Address kAipGetDispatchStubAddress = 0x0F000014U;
    static constexpr core::Address kAipCdDispatchStubAddress = 0x0F000010U;
    static constexpr core::Address kAipSetSh2DispatchStubAddress = 0x0F000018U;
    static constexpr core::Address kAipGetSh2DispatchStubAddress = 0x0F00001CU;
    static constexpr core::Address kAipChangeSystemClockStubAddress = 0x0F000020U;
    static constexpr core::Address kAipTasSemaphoreStubAddress = 0x0F000024U;
    static constexpr core::Address kAipClearSemaphoreStubAddress = 0x0F000028U;
    static constexpr core::Address kAipSetScuMaskStubAddress = 0x0F00002CU;

    BootRomServices();

    // By default SYS_EXECDMP marks the end of the deterministic boot harness.
    // Enable this mode to emulate the BIOS trampoline returning to the caller
    // through PR so the application can continue into its runtime.
    void setExecuteDmpReturns(bool enabled) noexcept;

    // Initializes the documented boot state, writes the service pointers into
    // sparse RAM, and materializes the corresponding host-owned SIR block.
    void install(sir::Module& module, sir::MachineState& state);

    void attachMmio(MmioInterceptor& mmio) noexcept;
    void setExecutionContext(const sir::ExecutionContext& context) override;
    [[nodiscard]] bool requiresStateSnapshot(core::Address address) const noexcept override;
    [[nodiscard]] core::Address backingAddress(core::Address address) const noexcept override;
    // Applies host-side effects for a materialized BIOS service block. This is
    // called by ProgramExecutor immediately before interpreting the block.
    void executeService(const sir::BasicBlock& block, sir::MachineState& state);

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) override;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                              std::uint32_t value) override;

    [[nodiscard]] sir::BasicBlock* resolve(core::Address target) const noexcept;
    [[nodiscard]] const std::vector<BootServiceSlot>& slots() const noexcept;
    [[nodiscard]] const std::vector<BootServiceCall>& calls() const noexcept;
    void clearCalls();
    [[nodiscard]] core::Address slaveEntryPoint() const noexcept { return slaveEntryPoint_; }

private:
    std::vector<BootServiceSlot> slots_;
    std::map<core::Address, sir::BasicBlock*> entrypoints_;
    MmioInterceptor* mmio_{};
    bool executeDmpReturns_{};
    core::Address slaveEntryPoint_{};
    std::uint16_t syncValue_{};
    std::vector<BootServiceCall> calls_;
};

} // namespace saturn::platform
