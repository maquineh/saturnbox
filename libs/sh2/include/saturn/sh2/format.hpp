#pragma once

#include "saturn/sh2/instruction.hpp"

#include <string>
#include <string_view>

namespace saturn::sh2 {

[[nodiscard]] std::string_view opcodeName(Opcode opcode) noexcept;
[[nodiscard]] std::string_view registerName(Register reg) noexcept;
[[nodiscard]] std::string_view flowName(FlowKind flow) noexcept;
[[nodiscard]] std::string_view delaySlotName(DelaySlot delaySlot) noexcept;
[[nodiscard]] std::string formatInstruction(const DecodedInstruction& instruction);
[[nodiscard]] std::string formatAddress(Address address);

} // namespace saturn::sh2
