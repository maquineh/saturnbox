#pragma once

#include "saturn/core/address.hpp"
#include "saturn/sir/sir.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace saturn::sir {

using StateSlot = std::uint16_t;

enum class Operation : std::uint8_t {
    ConstI1,
    ConstI32,
    CopyI32,
    AddI32,
    SubI32,
    MulI32,
    AddWithCarryI32,
    SubWithBorrowI32,
    AddWithOverflowI32,
    SubWithOverflowI32,
    AndI32,
    OrI32,
    XorI32,
    XorI1,
    NotI32,
    ShiftLeftI32,
    LogicalShiftRightI32,
    ArithmeticShiftRightI32,
    ExtractBitI32,
    TruncateI32ToI8,
    TruncateI32ToI16,
    SignExtendI8ToI32,
    SignExtendI16ToI32,
    ZeroExtendI1ToI32,
    CmpEqI32,
    CmpSignedGreaterEqualI32,
    CmpUnsignedGreaterEqualI32,
    CmpSignedGreaterI32,
    CmpUnsignedGreaterI32,
    DivideStepI32,
    Load8,
    Load16,
    Load32,
    Store8,
    Store16,
    Store32,
    ReadState,
    WriteState,
    Branch,
    BranchIf,
    BranchIndirect,
    Call,
    CallIndirect,
    Return,
    ReturnFromException,
    Trap,
    Sleep,
};

struct Instruction {
    Operation operation{Operation::ConstI32};
    std::vector<Value> results;
    std::vector<Value> operands;
    std::int64_t immediate{};
    StateSlot stateSlot{};
    BasicBlock* target{};
    BasicBlock* alternative{};
    BasicBlock* returnSite{};
    Function* function{};
};

class BasicBlock {
public:
    BasicBlock(core::Address address, std::string name);

    [[nodiscard]] core::Address address() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const std::vector<Instruction>& instructions() const noexcept;
    // Changes on append/reset so execution caches can detect a replaced body.
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool isLowered() const noexcept;
    void markLowered() noexcept;

private:
    friend class Module;
    friend class ModuleBuilder;
    core::Address address_{};
    std::string name_;
    std::vector<Instruction> instructions_;
    std::uint64_t revision_{};
    bool lowered_{};
};

class Function {
public:
    Function(core::Address address, std::string name);

    [[nodiscard]] core::Address address() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] BasicBlock* entry() const noexcept;
    void setEntry(BasicBlock* block) noexcept;

private:
    core::Address address_{};
    std::string name_;
    BasicBlock* entry_{};
};

class Module {
public:
    BasicBlock& createBlock(core::Address address, std::string name = {});
    Function& createFunction(core::Address address, std::string name = {});

    [[nodiscard]] BasicBlock* blockAt(core::Address address) noexcept;
    [[nodiscard]] const BasicBlock* blockAt(core::Address address) const noexcept;
    // Clear the current body while keeping its identity and incoming SIR
    // edges stable after an overlay replaces code at this guest address.
    [[nodiscard]] BasicBlock* resetBlockAt(core::Address address) noexcept;
    [[nodiscard]] Function* functionAt(core::Address address) noexcept;
    [[nodiscard]] const Function* functionAt(core::Address address) const noexcept;
    [[nodiscard]] std::size_t blockCount() const noexcept;
    [[nodiscard]] std::size_t functionCount() const noexcept;
    [[nodiscard]] std::size_t instructionCount() const noexcept;
    [[nodiscard]] const std::map<core::Address, std::unique_ptr<BasicBlock>>&
    blocks() const noexcept;

private:
    friend class ModuleBuilder;
    [[nodiscard]] Value allocateValue(Type type) noexcept;

    std::map<core::Address, std::unique_ptr<BasicBlock>> blocks_;
    std::map<core::Address, std::unique_ptr<Function>> functions_;
    std::uint32_t nextValueId_{1};
};

class ModuleBuilder final : public Builder {
public:
    explicit ModuleBuilder(Module& module) noexcept;

    void setInsertPoint(BasicBlock& block) noexcept;
    [[nodiscard]] BasicBlock* insertPoint() const noexcept;

    Value readState(StateSlot slot, Type type = Type::I32);
    void writeState(StateSlot slot, Value value);

    Value constI1(bool value) override;
    Value constI32(std::int32_t value) override;
    Value copyI32(Value value) override;
    Value addI32(Value left, Value right) override;
    Value subI32(Value left, Value right) override;
    Value mulI32(Value left, Value right) override;
    FlaggedValue addWithCarryI32(Value left, Value right, Value carryIn) override;
    FlaggedValue subWithBorrowI32(Value left, Value right, Value borrowIn) override;
    FlaggedValue addWithOverflowI32(Value left, Value right) override;
    FlaggedValue subWithOverflowI32(Value left, Value right) override;
    Value andI32(Value left, Value right) override;
    Value orI32(Value left, Value right) override;
    Value xorI32(Value left, Value right) override;
    Value xorI1(Value left, Value right) override;
    Value notI32(Value value) override;
    Value shiftLeftI32(Value value, Value amount) override;
    Value logicalShiftRightI32(Value value, Value amount) override;
    Value arithmeticShiftRightI32(Value value, Value amount) override;
    Value extractBitI32(Value value, std::uint8_t bit) override;
    Value truncateI32ToI8(Value value) override;
    Value truncateI32ToI16(Value value) override;
    Value signExtendI8ToI32(Value value) override;
    Value signExtendI16ToI32(Value value) override;
    Value zeroExtendI1ToI32(Value value) override;
    Value cmpEqI32(Value left, Value right) override;
    Value cmpSignedGreaterEqualI32(Value left, Value right) override;
    Value cmpUnsignedGreaterEqualI32(Value left, Value right) override;
    Value cmpSignedGreaterI32(Value left, Value right) override;
    Value cmpUnsignedGreaterI32(Value left, Value right) override;
    DivideStepResult divideStepI32(Value dividend, Value divisor,
                                   Value q, Value m, Value t) override;
    Value load8(Value address) override;
    Value load16(Value address) override;
    Value load32(Value address) override;
    void store8(Value address, Value value) override;
    void store16(Value address, Value value) override;
    void store32(Value address, Value value) override;
    void branch(BasicBlock* target) override;
    void branchIf(Value condition, BasicBlock* taken, BasicBlock* notTaken) override;
    void branchIndirect(Value target) override;
    void call(Function* target, BasicBlock* returnSite) override;
    void callIndirect(Value target, BasicBlock* returnSite) override;
    void ret(Value target) override;
    void returnFromException() override;
    void trap(std::uint8_t vector) override;
    void sleep() override;

private:
    Instruction& append(Operation operation);
    Value unary(Operation operation, Value operand, Type resultType);
    Value binary(Operation operation, Value left, Value right, Type resultType);
    FlaggedValue flagged(Operation operation, Value left, Value right);
    FlaggedValue flagged(Operation operation, Value left, Value right, Value flagIn);

    Module& module_;
    BasicBlock* block_{};
};

} // namespace saturn::sir
