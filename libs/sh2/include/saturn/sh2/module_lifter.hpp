#pragma once

#include "saturn/sh2/block_lifter.hpp"
#include "saturn/sir/module.hpp"

#include <cstddef>
#include <vector>

namespace saturn::sh2 {

[[nodiscard]] constexpr sir::StateSlot stateSlot(Register reg) noexcept {
    return static_cast<sir::StateSlot>(reg);
}

inline constexpr sir::StateSlot kStateSlotT = 23;
inline constexpr sir::StateSlot kStateSlotM = 24;
inline constexpr sir::StateSlot kStateSlotQ = 25;

struct ModuleLiftReport {
    std::size_t loweredBlocks{};
    std::size_t reusedBlocks{};
    std::vector<BlockLiftResult> failures;

    [[nodiscard]] bool success() const noexcept { return failures.empty(); }
};

// Pre-creates stable SIR block/function identities and lowers every recovered
// CFG block into executable SIR-L0 operations.
[[nodiscard]] ModuleLiftReport lowerCFG(const CFG& cfg, sir::Module& module);

} // namespace saturn::sh2
