#pragma once

#include "saturn/sh2/instruction.hpp"
#include "saturn/sir/sir.hpp"

namespace saturn::sh2 {

// Per-instruction lowering stays independent from CFG control flow. Block
// lowering composes this interface while preserving SH-2 delay-slot ordering.
class LiftContext {
public:
    virtual ~LiftContext() = default;
    [[nodiscard]] virtual sir::Value read(Register reg) = 0;
    virtual void write(Register reg, sir::Value value) = 0;
    [[nodiscard]] virtual sir::Value readT() = 0;
    virtual void writeT(sir::Value value) = 0;
    [[nodiscard]] virtual sir::Value readM() = 0;
    virtual void writeM(sir::Value value) = 0;
    [[nodiscard]] virtual sir::Value readQ() = 0;
    virtual void writeQ(sir::Value value) = 0;
    [[nodiscard]] virtual Address sourcePC() const = 0;
    [[nodiscard]] virtual sir::Builder& builder() = 0;
};

enum class LiftResult { Success, Unsupported };
using LiftFn = LiftResult (*)(const DecodedInstruction&, LiftContext&);

[[nodiscard]] LiftFn lifterFor(Opcode opcode) noexcept;
[[nodiscard]] LiftResult liftInstruction(const DecodedInstruction& instruction,
                                         LiftContext& context);

} // namespace saturn::sh2
