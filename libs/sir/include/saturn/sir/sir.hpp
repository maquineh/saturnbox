#pragma once

#include <cstdint>
#include <string_view>

namespace saturn::sir {

// SIR stays independent from LLVM and from the eventual PC runtime.
enum class Type : std::uint8_t {
    Void,
    I1,
    I8,
    I16,
    I32,
    I64,
    Address,
};

struct Value {
    std::uint32_t id{};
    Type type{Type::Void};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return type != Type::Void; }
};

struct FlaggedValue {
    Value value;
    Value flag;
};

struct DivideStepResult {
    Value value;
    Value q;
    Value t;
};

class BasicBlock;
class Function;

// Contract consumed by ISA lifters in later milestones. It deliberately makes
// no assumptions about LLVM, a JIT, or a host graphics API.
class Builder {
public:
    virtual ~Builder() = default;

    virtual Value constI1(bool value) = 0;
    virtual Value constI32(std::int32_t value) = 0;
    virtual Value copyI32(Value value) = 0;
    virtual Value addI32(Value left, Value right) = 0;
    virtual Value subI32(Value left, Value right) = 0;
    virtual Value mulI32(Value left, Value right) = 0;
    virtual FlaggedValue addWithCarryI32(Value left, Value right, Value carryIn) = 0;
    virtual FlaggedValue subWithBorrowI32(Value left, Value right, Value borrowIn) = 0;
    virtual FlaggedValue addWithOverflowI32(Value left, Value right) = 0;
    virtual FlaggedValue subWithOverflowI32(Value left, Value right) = 0;
    virtual Value andI32(Value left, Value right) = 0;
    virtual Value orI32(Value left, Value right) = 0;
    virtual Value xorI32(Value left, Value right) = 0;
    virtual Value xorI1(Value left, Value right) = 0;
    virtual Value notI32(Value value) = 0;
    virtual Value shiftLeftI32(Value value, Value amount) = 0;
    virtual Value logicalShiftRightI32(Value value, Value amount) = 0;
    virtual Value arithmeticShiftRightI32(Value value, Value amount) = 0;
    virtual Value extractBitI32(Value value, std::uint8_t bit) = 0;
    virtual Value truncateI32ToI8(Value value) = 0;
    virtual Value truncateI32ToI16(Value value) = 0;
    virtual Value signExtendI8ToI32(Value value) = 0;
    virtual Value signExtendI16ToI32(Value value) = 0;
    virtual Value zeroExtendI1ToI32(Value value) = 0;
    virtual Value cmpEqI32(Value left, Value right) = 0;
    virtual Value cmpSignedGreaterEqualI32(Value left, Value right) = 0;
    virtual Value cmpUnsignedGreaterEqualI32(Value left, Value right) = 0;
    virtual Value cmpSignedGreaterI32(Value left, Value right) = 0;
    virtual Value cmpUnsignedGreaterI32(Value left, Value right) = 0;
    virtual DivideStepResult divideStepI32(Value dividend, Value divisor,
                                           Value q, Value m, Value t) = 0;
    virtual Value load8(Value address) = 0;
    virtual Value load16(Value address) = 0;
    virtual Value load32(Value address) = 0;
    virtual void store8(Value address, Value value) = 0;
    virtual void store16(Value address, Value value) = 0;
    virtual void store32(Value address, Value value) = 0;
    virtual void branch(BasicBlock* target) = 0;
    virtual void branchIf(Value condition, BasicBlock* taken, BasicBlock* notTaken) = 0;
    virtual void branchIndirect(Value target) = 0;
    virtual void call(Function* target, BasicBlock* returnSite) = 0;
    virtual void callIndirect(Value target, BasicBlock* returnSite) = 0;
    virtual void ret(Value target) = 0;
    virtual void returnFromException() = 0;
    virtual void trap(std::uint8_t vector) = 0;
    virtual void sleep() = 0;
};

[[nodiscard]] std::string_view version() noexcept;

} // namespace saturn::sir
