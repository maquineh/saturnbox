#pragma once

#include "saturn/core/address.hpp"

#include <cstdint>

namespace saturn::sh2 {

using Address = core::Address;

enum class Register : std::uint8_t {
    R0, R1, R2, R3, R4, R5, R6, R7,
    R8, R9, R10, R11, R12, R13, R14, R15,
    PC, PR, GBR, VBR, MACH, MACL, SR,
};

[[nodiscard]] constexpr Register generalRegister(std::uint8_t value) noexcept {
    return static_cast<Register>(value & 0x0FU);
}

enum class FlowKind : std::uint8_t {
    Fallthrough,
    DirectBranch,
    ConditionalBranch,
    IndirectBranch,
    DirectCall,
    IndirectCall,
    Return,
    ExceptionReturn,
    Trap,
};

enum class DelaySlot : std::uint8_t {
    None,
    Always,
    // Reserved for an ISA whose slot is executed only on the taken path.
    // SH-2 BT/S and BF/S use Always: both paths execute their slot.
    Taken,
};

enum class Confidence : std::uint8_t {
    Proven,
    High,
    Medium,
    Low,
    Unknown,
};

} // namespace saturn::sh2
