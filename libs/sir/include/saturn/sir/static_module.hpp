#pragma once

#include "saturn/sir/interpreter.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace saturn::sir {

struct StaticBlockSignature {
    std::uint64_t first{};
    std::uint64_t second{};

    [[nodiscard]] constexpr bool operator==(const StaticBlockSignature&) const noexcept = default;
};

using StaticBlockFunction = ExecutionResult (*)(const Module&, MachineState&);

struct StaticNativeBlockEntry {
    core::Address address{};
    StaticBlockSignature signature{};
    StaticBlockFunction function{};
};

[[nodiscard]] StaticBlockSignature staticBlockSignature(
    const BasicBlock& block);

class StaticNativeBlockExecutor final {
public:
    StaticNativeBlockExecutor(const Module& module,
                              std::span<const StaticNativeBlockEntry> blocks) noexcept;

    // A missing address or a different SIR shape returns nullopt so the
    // caller can use the JIT or reference interpreter for dynamic overlays.
    [[nodiscard]] std::optional<ExecutionResult> execute(
        const BasicBlock& block, MachineState& state);
    [[nodiscard]] std::size_t blockCount() const noexcept;
    [[nodiscard]] std::size_t executedBlockCount() const noexcept;
    [[nodiscard]] const std::vector<core::Address>& overlayVariantSelections() const noexcept;

private:
    struct CachedResolution {
        std::uint64_t revision{};
        StaticBlockSignature signature{};
        StaticBlockFunction function{};
    };

    const Module& module_;
    std::span<const StaticNativeBlockEntry> blocks_;
    std::unordered_map<const BasicBlock*, CachedResolution> resolved_;
    std::size_t executedBlockCount_{};
    std::vector<core::Address> overlayVariantSelections_;
};

struct StaticModuleEmitReport {
    std::size_t blocksEmitted{};
    std::size_t instructionsEmitted{};
    std::vector<core::Address> skippedBlocks;
    std::string error;

    [[nodiscard]] bool success() const noexcept { return error.empty(); }
};

struct StaticModuleEmitOptions {
    // When set, emit only blocks from the 64 KiB guest address bank. Keeping
    // bank boundaries stable lets generated translation units be reused when
    // unrelated guest code changes.
    std::optional<std::uint16_t> addressBank;
    std::string_view registryFunction{"saturnbox_static_blocks"};
};

// Emits a C++ translation unit containing one host-compiled function per
// lowered SIR block and a sorted address/signature table. Compile the result
// with the same C++ ABI and link it into the Saturn runtime.
[[nodiscard]] StaticModuleEmitReport emitStaticModuleCpp(
    const Module& module, std::ostream& output);

// Emits image versions into one lookup table. Blocks sharing a guest address
// remain distinct when their SIR signatures differ, allowing the active
// runtime module to select the compiled version matching a loaded overlay.
[[nodiscard]] StaticModuleEmitReport emitStaticModulesCpp(
    std::span<const Module* const> modules, std::ostream& output);

[[nodiscard]] StaticModuleEmitReport emitStaticModulesCpp(
    std::span<const Module* const> modules, std::ostream& output,
    const StaticModuleEmitOptions& options);

} // namespace saturn::sir
