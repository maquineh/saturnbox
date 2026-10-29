#pragma once

#include "saturn/sir/module.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace saturn::sir {

struct ExecutionContext {
    core::Address blockAddress{};
    core::Address callerBlock{};
    std::size_t step{};
    std::map<StateSlot, std::uint32_t> state;
};

class MemoryInterceptor {
public:
    virtual ~MemoryInterceptor() = default;

    virtual void setExecutionContext(const ExecutionContext&) {}
    // Tracing consumers keep the register snapshot; live devices can use only
    // block/caller/step and avoid allocating a map at every CPU boundary.
    [[nodiscard]] virtual bool requiresStateSnapshot(core::Address) const noexcept { return true; }
    // Resolve a guest address to its backing RAM address after device hooks
    // decline the access. Other memory models keep the address unchanged.
    [[nodiscard]] virtual core::Address backingAddress(core::Address address) const noexcept {
        return address;
    }

    // A value handles the read; std::nullopt falls back to sparse RAM.
    [[nodiscard]] virtual std::optional<std::uint32_t> read(core::Address address,
                                                             std::uint8_t width) = 0;
    // True handles the write and suppresses the sparse-RAM write.
    [[nodiscard]] virtual bool write(core::Address address, std::uint8_t width,
                                     std::uint32_t value) = 0;
};

class MachineState {
public:
    static constexpr std::size_t kDenseStateSlots = 64U;
    [[nodiscard]] std::uint32_t readState(StateSlot slot) const noexcept;
    void writeState(StateSlot slot, std::uint32_t value);
    // Native code writes these slots directly. MMIO hooks see the same values;
    // stateSlots() rebuilds its diagnostic snapshot when requested.
    [[nodiscard]] std::uint32_t* nativeStateData() noexcept;

    [[nodiscard]] std::uint8_t read8(core::Address address) const;
    [[nodiscard]] std::uint16_t read16BE(core::Address address) const;
    [[nodiscard]] std::uint32_t read32BE(core::Address address) const;
    void write8(core::Address address, std::uint8_t value);
    void write16BE(core::Address address, std::uint16_t value);
    void write32BE(core::Address address, std::uint32_t value);
    // Generation of the 4 KiB backing-RAM page containing address. Runtime
    // block caches use this to notice self-modifying code and loaded overlays.
    [[nodiscard]] std::uint64_t memoryWriteGeneration(
        core::Address address) const noexcept;
    [[nodiscard]] std::uint64_t memoryWriteEpoch() const noexcept;
    void setMemoryInterceptor(MemoryInterceptor* interceptor) noexcept;
    [[nodiscard]] MemoryInterceptor* memoryInterceptor() const noexcept;
    void setExecutionContext(const ExecutionContext& context);

    [[nodiscard]] const std::map<StateSlot, std::uint32_t>& stateSlots() const;
    // Materializes a diagnostic snapshot; obtain it again after guest writes.
    [[nodiscard]] const std::map<core::Address, std::uint8_t>& memory() const;
    // Observer access to backing RAM without recursively invoking MMIO hooks.
    [[nodiscard]] std::uint8_t readRaw8(core::Address address) const noexcept;
    [[nodiscard]] bool operator==(const MachineState& other) const;

private:
    struct RawPageReadCache {
        static constexpr std::size_t kEntryCount = 64U;
        struct Entry {
            core::Address pageKey{};
            const std::vector<std::uint8_t>* bytes{};
            bool valid{};
        };

        RawPageReadCache() = default;
        RawPageReadCache(const RawPageReadCache&) noexcept {}
        RawPageReadCache& operator=(const RawPageReadCache&) noexcept {
            clear();
            return *this;
        }
        RawPageReadCache(RawPageReadCache&& other) noexcept { other.clear(); }
        RawPageReadCache& operator=(RawPageReadCache&& other) noexcept {
            clear();
            other.clear();
            return *this;
        }

        void clear() noexcept { entries = {}; }

        std::array<Entry, kEntryCount> entries{};
    };

    [[nodiscard]] core::Address backingAddress(core::Address address) const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>* rawPageData(
        core::Address pageKey) const noexcept;
    void cacheRawPage(core::Address pageKey,
                      const std::vector<std::uint8_t>* bytes) const noexcept;
    void noteMemoryWrite(core::Address address);
    void writeRaw8(core::Address address, std::uint8_t value);
    void writeRawWord(core::Address address, std::uint32_t value, std::uint8_t width);

    mutable std::map<StateSlot, std::uint32_t> state_;
    mutable bool stateSnapshotDirty_{};
    bool nativeStateExposed_{};
    mutable std::map<core::Address, std::uint8_t> memory_;
    mutable bool memorySnapshotDirty_{};
    // Dense storage allocates on the first nonzero write and copies with the
    // machine state. The canonical zero-eliding map is diagnostic only.
    std::array<std::uint32_t, kDenseStateSlots> fastState_{};
    std::unordered_map<core::Address, std::vector<std::uint8_t>> memoryPages_;
    mutable RawPageReadCache rawPageReadCache_;
    std::unordered_map<core::Address, std::uint64_t> memoryPageWriteGenerations_;
    std::uint64_t memoryWriteEpoch_{};
    MemoryInterceptor* interceptor_{};
};

// Reserved state slots used by the block executor's SH-2 exception bridge.
// The sh2 library maps its Register enum to these same slots.
inline constexpr StateSlot kStackPointerStateSlot = 15;
inline constexpr StateSlot kProgramCounterStateSlot = 16;
inline constexpr StateSlot kProcedureRegisterStateSlot = 17;
inline constexpr StateSlot kVectorBaseStateSlot = 19;
inline constexpr StateSlot kStatusRegisterStateSlot = 22;

enum class ControlKind : std::uint8_t {
    None,
    Branch,
    IndirectBranch,
    Call,
    IndirectCall,
    Return,
    ExceptionReturn,
    Trap,
};

enum class InterpreterStatus : std::uint8_t {
    Success,
    MissingValue,
    MalformedInstruction,
    MissingTerminator,
};

struct ExecutionResult {
    InterpreterStatus status{InterpreterStatus::Success};
    ControlKind control{ControlKind::None};
    const BasicBlock* target{};
    const Function* function{};
    const BasicBlock* returnSite{};
    std::uint32_t dynamicTarget{};
    std::uint8_t trapVector{};
    bool slept{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == InterpreterStatus::Success;
    }
};

class Interpreter {
public:
    [[nodiscard]] ExecutionResult execute(const BasicBlock& block,
                                          MachineState& state) const;
};

using BlockResolver = std::function<BasicBlock*(core::Address)>;
using StepAwareBlockResolver = std::function<BasicBlock*(core::Address, std::size_t)>;
using BlockObserver = std::function<void(const BasicBlock&, std::size_t)>;
using BlockExecutionHook = std::function<std::optional<ExecutionResult>(
    const BasicBlock&, MachineState&)>;
struct BlockChainResult {
    ExecutionResult last;
    const BasicBlock* lastBlock{};
    std::size_t blocksExecuted{};
};
// Called once after every completed native block. The program executor owns
// control flow, boundary hooks and interrupts and returns the prepared next
// block, or nullptr when execution must stop. No block may run without this
// callback having prepared its boundary (except the already-prepared entry).
using BlockChainContinuation = std::function<const BasicBlock*(
    const BasicBlock&, const ExecutionResult&)>;
// A bounded chain must stop before an unsupported successor; the continuation
// may already have prepared that successor for interpreter fallback.
using BlockChainExecutionHook = std::function<std::optional<BlockChainResult>(
    const BasicBlock&, MachineState&, std::size_t, const BlockChainContinuation&)>;
// Called after the execution context is published and before a block runs.
// Host runtimes use this for side effects which are attached to a materialized
// block entry (for example, a BIOS service dispatch).
using BlockHook = std::function<void(const BasicBlock&, MachineState&, std::size_t)>;
// Called at a block boundary before BlockHook. Returning a target enters an
// asynchronous exception handler; the executor saves PC/SR on the SH-2 stack
// and resumes the interrupted block when a hardware handler executes RTE or
// when a firmware-managed callback returns with RTS.
enum class InterruptReturnMode : std::uint8_t {
    Exception,
    Callback,
};

struct InterruptRequest {
    core::Address target{};
    InterruptReturnMode returnMode{InterruptReturnMode::Exception};
};

using InterruptHook = std::function<std::optional<InterruptRequest>(
    const BasicBlock&, MachineState&, std::size_t)>;

struct ProgramExecutionOptions {
    std::size_t maxSteps{100'000};
    BlockResolver resolveBlock;
    StepAwareBlockResolver resolveBlockAtStep;
    // Re-resolve even known direct branch targets when their backing code
    // pages have changed. This supports overlays loaded during a run.
    bool refreshKnownBlocksOnWrite{};
    // A backend may execute a block and return its result. Returning nullopt
    // leaves the block to the reference interpreter.
    BlockExecutionHook executeBlock;
    // The continuation preserves every execution context, scheduler event
    // and interrupt check while a backend tail-chains native blocks.
    BlockChainExecutionHook executeChain;
    BlockHook beforeBlock;
    BlockObserver observeBlock;
    InterruptHook interrupt;
};

enum class ProgramStatus : std::uint8_t {
    Returned,
    StepLimit,
    MissingEntry,
    UnresolvedTarget,
    InterpreterFailure,
    Trap,
    ExceptionReturn,
    Sleeping,
    FrontendQuit,
};

struct ProgramExecutionResult {
    ProgramStatus status{ProgramStatus::MissingEntry};
    std::size_t steps{};
    core::Address address{};
    core::Address unresolvedTarget{};
    ExecutionResult last;
};

// Executes a chain of basic blocks. A step is one completed SIR block, so the
// caller can bound loops independently from the number of SIR operations.
class ProgramExecutor {
public:
    [[nodiscard]] ProgramExecutionResult run(BasicBlock* entry, MachineState& state,
                                             const ProgramExecutionOptions& options = {}) const;
};

} // namespace saturn::sir
