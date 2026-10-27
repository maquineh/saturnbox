#pragma once

#include "saturn/core/memory_map.hpp"
#include "saturn/sh2/module_lifter.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace saturn::sir { class MachineState; }

namespace saturn::sh2 {

struct DynamicResolutionStats {
    std::size_t requests{};
    std::size_t reusedBlocks{};
    std::size_t loweredBlocks{};
    std::size_t replacedBlocks{};
    std::size_t failures{};
};

struct RuntimeOverlayReplacement {
    Address address{};
    std::size_t step{};
    std::uint64_t previousSignature{};
    std::uint64_t replacementSignature{};
    bool previousSignatureKnown{};
};

// Resolves runtime addresses to executable SIR. Live instruction fingerprints
// let a newly loaded overlay replace a stale body at the same guest address.
class RuntimeBlockResolver {
public:
    RuntimeBlockResolver(const core::MemoryMap& image, sir::Module& module,
                         const sir::MachineState* runtimeMemory = nullptr);

    [[nodiscard]] sir::BasicBlock* resolve(Address address, std::size_t step = 0U);
    [[nodiscard]] const DynamicResolutionStats& stats() const noexcept;
    [[nodiscard]] std::vector<Address> resolvedAddresses() const;
    [[nodiscard]] const std::vector<Address>& replacedAddresses() const noexcept;
    [[nodiscard]] const std::vector<RuntimeOverlayReplacement>& replacements() const noexcept;

private:
    const core::MemoryMap& image_;
    sir::Module& module_;
    const sir::MachineState* runtimeMemory_{};
    // Indirect dispatch is hot in the Saturn runtime (SCU/CD/VDP callbacks
    // revisit the same targets every frame). Keep a direct address cache so
    // the common path avoids repeated module map lookups.
    std::unordered_map<Address, sir::BasicBlock*> blockCache_;
    std::unordered_map<Address, std::uint64_t> blockCodeSignatures_;
    std::unordered_map<Address, std::uint64_t> failedCodeSignatures_;
    std::vector<Address> replacedAddresses_;
    std::vector<RuntimeOverlayReplacement> replacements_;
    DynamicResolutionStats stats_;
};

} // namespace saturn::sh2
