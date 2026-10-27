#pragma once

#include "saturn/sir/interpreter.hpp"

#include <cstddef>
#include <memory>
#include <optional>

namespace saturn::sir {

// Small demand-compiled x64 backend with MachineState memory/MMIO helpers.
// Unsupported blocks return nullopt for ProgramExecutor's interpreter fallback.
class Win64JitBlockExecutor final {
public:
    Win64JitBlockExecutor();
    ~Win64JitBlockExecutor();

    Win64JitBlockExecutor(const Win64JitBlockExecutor&) = delete;
    Win64JitBlockExecutor& operator=(const Win64JitBlockExecutor&) = delete;

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] std::optional<ExecutionResult> execute(const BasicBlock& block,
                                                          MachineState& state);
    // With a continuation, ProgramExecutor prepares every successor boundary,
    // including MMIO and interrupts. Without it, only known direct transfers
    // without a memory interceptor can be chained.
    [[nodiscard]] std::optional<BlockChainResult> executeChain(
        const BasicBlock& entry, MachineState& state, std::size_t maxBlocks,
        const BlockChainContinuation& continuation = {});
    [[nodiscard]] std::size_t compiledBlockCount() const noexcept;
    [[nodiscard]] std::size_t executedBlockCount() const noexcept;
    [[nodiscard]] std::size_t chainedTransferCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saturn::sir
