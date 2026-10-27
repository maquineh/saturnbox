#pragma once

#include "saturn/saturn/platform.hpp"
#include "saturn/saturn/cd.hpp"
#include "saturn/saturn/dma.hpp"
#include "saturn/saturn/smpc.hpp"
#include "saturn/saturn/scsp.hpp"
#include "saturn/saturn/video.hpp"
#include "saturn/sir/interpreter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>

namespace saturn::platform {

struct MmioAccess {
    core::Address address{};
    std::uint8_t width{};
    std::uint32_t value{};
    AddressSpace space{AddressSpace::Unknown};
    bool write{};
    sir::ExecutionContext context;
};

enum class ControlTraceKind : std::uint8_t {
    PadInput,
    SmpcRead,
    SmpcWrite,
    CdCommand,
};

struct ControlTraceRecord {
    std::uint64_t sequence{};
    std::size_t step{};
    core::Address block{};
    core::Address caller{};
    core::Address address{};
    std::uint8_t width{};
    std::uint32_t value{};
    ControlTraceKind kind{ControlTraceKind::SmpcRead};
    std::array<std::uint16_t, 4> cdRegisters{};
    std::uint8_t port{};
    bool connected{};
};

// A deterministic placeholder for Saturn peripherals. It handles only known
// MMIO regions, records accesses and exposes register values to the CPU; RAM
// accesses fall through to MachineState's sparse backing store.
class MmioInterceptor final : public sir::MemoryInterceptor {
public:
    // SCU timer registers are exposed through both the cached (0x05FE...)
    // and cache-bypass (0x25FE...) bus aliases.  Timer 0 uses the HBlank
    // scanline counter and is enabled by T1MD.T0EN (bit 8).
    static constexpr core::Address kScuTimer0CompareRegister = 0x05FE0090U;
    static constexpr core::Address kScuTimerModeRegister = 0x05FE0098U;
    static constexpr core::Address kSh2FreeRunningCounter = 0xFFFFFE12U;
    static constexpr core::Address kSh2FreeRunningTimerControl = 0xFFFFFE16U;

    void setExecutionContext(const sir::ExecutionContext& context) override;
    // Match the FRT clock to the scheduler's guest-time block budget.
    void setCpuTiming(std::uint32_t cyclesPerSecond, std::size_t blocksPerSecond) noexcept;
    void captureFrtInput() noexcept;
    [[nodiscard]] bool takeSlaveInputCapture() noexcept;
    [[nodiscard]] const sir::ExecutionContext& executionContext() const noexcept { return context_; }
    [[nodiscard]] bool requiresStateSnapshot(core::Address) const noexcept override { return accessRecording_; }
    [[nodiscard]] core::Address backingAddress(core::Address address) const noexcept override;
    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) override;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                             std::uint32_t value) override;

    void setReadValue(core::Address address, std::uint8_t width, std::uint32_t value);
    void setAccessRecording(bool enabled) noexcept;
    void setAccessHistoryLimit(std::size_t limit) noexcept;
    void clearAccesses();
    [[nodiscard]] const std::deque<MmioAccess>& accesses() const noexcept;
    [[nodiscard]] std::size_t accessCount() const noexcept;
    void setControlTraceEnabled(bool enabled) noexcept;
    void setControlTraceStartStep(std::size_t step) noexcept;
    void setControlTraceOreg2Only(bool enabled) noexcept;
    void recordPadInput(std::size_t step, std::size_t port,
                        const DigitalPadState& state);
    [[nodiscard]] const std::deque<ControlTraceRecord>& controlTrace() const noexcept;
    [[nodiscard]] std::size_t droppedControlTraceRecords() const noexcept;
    [[nodiscard]] SmpcRuntime& smpc() noexcept;
    [[nodiscard]] const SmpcRuntime& smpc() const noexcept;
    [[nodiscard]] CdRuntime& cd() noexcept;
    [[nodiscard]] const CdRuntime& cd() const noexcept;
    [[nodiscard]] VideoRuntime& video() noexcept;
    [[nodiscard]] const VideoRuntime& video() const noexcept;
    [[nodiscard]] ScuDmaRuntime& scuDma() noexcept;
    [[nodiscard]] const ScuDmaRuntime& scuDma() const noexcept;
    [[nodiscard]] ScspRuntime& scsp() noexcept;
    [[nodiscard]] const ScspRuntime& scsp() const noexcept;
    void attachMachineState(sir::MachineState& state) noexcept;
    // Advances SH-2 built-in DMA channels at a scheduler boundary. The game
    // uses channel 0 for a small work-RAM transfer and polls CHCR.TE before
    // enabling the first display list.
    void onSchedulerTick(sir::MachineState& state);

    // SCU interrupt-factor helpers used by the host event scheduler. They
    // model the low-word status/mask registers without fabricating CPU work;
    // IST is cleared only through this explicit event-side acknowledge.
    void raiseScuInterrupt(std::uint32_t factor) noexcept;
    void clearScuInterrupt(std::uint32_t factor) noexcept;
    [[nodiscard]] std::uint32_t scuInterruptStatus() const noexcept;
    [[nodiscard]] std::uint32_t scuInterruptMask() const noexcept;
    [[nodiscard]] std::uint32_t scuInterruptPending() const noexcept;
    [[nodiscard]] std::uint16_t scuTimer0Compare() const noexcept;
    [[nodiscard]] bool scuTimer0Enabled() const noexcept;

private:
    using RegisterKey = std::pair<core::Address, std::uint8_t>;
    // Each entry includes a copied SH-2 context, so keep only recent history.
    static constexpr std::size_t kDefaultAccessHistoryLimit = 262144U;

    [[nodiscard]] static bool isMmio(AddressSpace space) noexcept;
    void recordAccess(core::Address address, std::uint8_t width,
                      std::uint32_t value, AddressSpace space, bool write);
    void recordControlAccess(core::Address address, std::uint8_t width,
                             std::uint32_t value, AddressSpace space, bool write);
    void appendControlTrace(ControlTraceRecord record);

    std::map<RegisterKey, std::uint32_t> registers_;
    std::deque<MmioAccess> accesses_;
    std::size_t accessHistoryLimit_{kDefaultAccessHistoryLimit};
    std::size_t accessCount_{};
    bool accessRecording_{true};
    static constexpr std::size_t kControlTraceHistoryLimit = 262144U;
    std::deque<ControlTraceRecord> controlTrace_;
    std::uint64_t controlTraceSequence_{};
    std::size_t droppedControlTraceRecords_{};
    std::size_t controlTraceStartStep_{};
    bool controlTraceOreg2Only_{};
    bool controlTraceEnabled_{};
    SmpcRuntime smpc_;
    CdRuntime cd_;
    ScuDmaRuntime scuDma_;
    ScspRuntime scsp_;
    VideoRuntime video_;
    sir::ExecutionContext context_;
    std::uint32_t scuInterruptMask_{0xFFFFFFFFU};
    std::uint32_t scuInterruptStatus_{};
    std::uint32_t abusInterruptAcknowledge_{};
    sir::MachineState* machineState_{};
    std::uint16_t scuTimer0Compare_{};
    std::uint32_t scuTimerMode_{};
    // SH-2 on-chip division unit.  The boot/runtime code uses this window
    // while calculating ISO directory sector counts; it is not backed by
    // Saturn RAM and therefore needs explicit register semantics.
    std::uint32_t sh2DivisionDivisor_{};
    std::uint32_t sh2DivisionDividendLow_{};
    std::uint32_t sh2DivisionDividendHigh_{};
    std::uint32_t sh2DivisionControl_{};
    // Unconfigured fixtures use eight cycles/block. The launcher supplies
    // the CPU clock and display budget; fractional ticks survive boundaries.
    std::array<std::uint8_t, 10> sh2FrtRegisters_{};
    std::uint16_t sh2FreeRunningCounter_{};
    std::size_t sh2FrtLastStep_{};
    std::uint64_t sh2FrtBlockRemainder_{};
    std::uint32_t sh2CyclesPerSecond_{8U};
    std::size_t sh2BlocksPerSecond_{1U};
    bool sh2FrtStepInitialized_{};
    bool slaveInputCapture_{};
    std::uint32_t sh2DmaSar0_{};
    std::uint32_t sh2DmaDar0_{};
    std::uint32_t sh2DmaTcr0_{};
    std::uint32_t sh2DmaChcr0_{};
    std::uint32_t sh2DmaSar1_{};
    std::uint32_t sh2DmaDar1_{};
    std::uint32_t sh2DmaTcr1_{};
    std::uint32_t sh2DmaChcr1_{};
    std::uint32_t sh2DmaVcr0_{};
    std::uint32_t sh2DmaVcr1_{};
    std::uint32_t sh2DmaOr_{};
};

} // namespace saturn::platform
