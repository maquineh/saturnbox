#pragma once

#include "saturn/sh2/types.hpp"

#include <cstdint>
#include <optional>
#include <variant>

namespace saturn::sh2 {

struct RegisterOperand {
    Register reg{Register::R0};
};

struct ImmediateOperand {
    std::int32_t value{};
    std::uint8_t bits{};
    bool signExtended{};
};

struct DisplacementOperand {
    std::int32_t value{};
};

struct AddressOperand {
    Address address{};
};

struct MemoryOperand {
    Register base{Register::R0};
    std::optional<Register> index;
    std::int32_t displacement{};
    std::uint8_t width{};
    enum class Mode : std::uint8_t { Indirect, PreDecrement, PostIncrement } mode{Mode::Indirect};
};

using Operand = std::variant<RegisterOperand, ImmediateOperand, DisplacementOperand,
                             AddressOperand, MemoryOperand>;

} // namespace saturn::sh2
