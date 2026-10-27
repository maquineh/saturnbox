#pragma once

#include "saturn/sh2/opcode.hpp"
#include "saturn/sh2/operand.hpp"
#include "saturn/sh2/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace saturn::sh2 {

struct CycleInfo {
    std::uint8_t minimum{1};
    std::uint8_t maximum{1};
};

struct DecodedInstruction {
    Address address{};
    std::uint16_t raw{};
    Opcode opcode{Opcode::Invalid};
    std::array<Operand, 3> operands{};
    std::uint8_t operandCount{};
    FlowKind flow{FlowKind::Trap};
    DelaySlot delaySlot{DelaySlot::None};
    std::uint8_t length{2};
    CycleInfo cycles{};
    bool valid{};

    // Direct control transfers expose their resolved architectural target so
    // CFG and future lifter code never repeat PC-relative arithmetic.
    std::optional<Address> directTarget;

    [[nodiscard]] const Operand& operand(std::size_t index) const;
};

} // namespace saturn::sh2
