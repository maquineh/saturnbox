#pragma once

#include "saturn/sh2/cfg.hpp"
#include "saturn/sh2/lifter.hpp"

#include <string_view>

namespace saturn::sh2 {

class BlockLiftContext : public LiftContext {
public:
    [[nodiscard]] virtual sir::BasicBlock* blockFor(Address address) = 0;
    [[nodiscard]] virtual sir::Function* functionFor(Address address) = 0;
};

enum class BlockLiftStatus : std::uint8_t {
    Success,
    InvalidInstruction,
    UnsupportedInstruction,
    MissingDelaySlot,
    InvalidDelaySlot,
    MissingSuccessor,
    MissingTarget,
};

struct BlockLiftResult {
    BlockLiftStatus status{BlockLiftStatus::Success};
    Address address{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == BlockLiftStatus::Success;
    }
};

// Lowers one recovered SH-2 block. Values that decide a delayed transfer are
// captured before its slot, while the transfer itself is emitted afterwards.
[[nodiscard]] BlockLiftResult lowerBlock(const BasicBlock& block,
                                         BlockLiftContext& context);
[[nodiscard]] std::string_view blockLiftStatusName(BlockLiftStatus status) noexcept;

} // namespace saturn::sh2
