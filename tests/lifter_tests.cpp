#include "test_support.hpp"

#include "saturn/sh2/block_lifter.hpp"
#include "saturn/sh2/decoder.hpp"
#include "saturn/sh2/lifter.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace saturn;

class RecordingBuilder final : public sir::Builder {
public:
    std::vector<std::string> operations;

    sir::Value constI1(bool value) override {
        return emit(std::string("const.i1:") + (value ? "1" : "0"), sir::Type::I1);
    }
    sir::Value constI32(std::int32_t value) override {
        return emit("const:" + std::to_string(value), sir::Type::I32);
    }
    sir::Value copyI32(sir::Value value) override { return unary("copy", value); }
    sir::Value addI32(sir::Value left, sir::Value right) override {
        return binary("add", left, right);
    }
    sir::Value subI32(sir::Value left, sir::Value right) override {
        return binary("sub", left, right);
    }
    sir::Value mulI32(sir::Value left, sir::Value right) override {
        return binary("mul", left, right);
    }
    sir::FlaggedValue addWithCarryI32(sir::Value left, sir::Value right,
                                      sir::Value carryIn) override {
        return flagged("addc", left, right, carryIn);
    }
    sir::FlaggedValue subWithBorrowI32(sir::Value left, sir::Value right,
                                       sir::Value borrowIn) override {
        return flagged("subc", left, right, borrowIn);
    }
    sir::FlaggedValue addWithOverflowI32(sir::Value left, sir::Value right) override {
        return flagged("addv", left, right);
    }
    sir::FlaggedValue subWithOverflowI32(sir::Value left, sir::Value right) override {
        return flagged("subv", left, right);
    }
    sir::Value andI32(sir::Value left, sir::Value right) override {
        return binary("and", left, right);
    }
    sir::Value orI32(sir::Value left, sir::Value right) override {
        return binary("or", left, right);
    }
    sir::Value xorI32(sir::Value left, sir::Value right) override {
        return binary("xor", left, right);
    }
    sir::Value xorI1(sir::Value left, sir::Value right) override {
        return binary("xor.i1", left, right, sir::Type::I1);
    }
    sir::Value notI32(sir::Value value) override { return unary("not", value); }
    sir::Value shiftLeftI32(sir::Value value, sir::Value amount) override {
        return binary("shl", value, amount);
    }
    sir::Value logicalShiftRightI32(sir::Value value, sir::Value amount) override {
        return binary("lshr", value, amount);
    }
    sir::Value arithmeticShiftRightI32(sir::Value value, sir::Value amount) override {
        return binary("ashr", value, amount);
    }
    sir::Value extractBitI32(sir::Value value, std::uint8_t bit) override {
        return emit("bit:" + std::to_string(value.id) + ':' + std::to_string(bit),
                    sir::Type::I1);
    }
    sir::Value truncateI32ToI8(sir::Value value) override {
        return unary("trunc8", value, sir::Type::I8);
    }
    sir::Value truncateI32ToI16(sir::Value value) override {
        return unary("trunc16", value, sir::Type::I16);
    }
    sir::Value signExtendI8ToI32(sir::Value value) override {
        return unary("sext8", value);
    }
    sir::Value signExtendI16ToI32(sir::Value value) override {
        return unary("sext16", value);
    }
    sir::Value zeroExtendI1ToI32(sir::Value value) override {
        return unary("zext1", value);
    }
    sir::Value cmpEqI32(sir::Value left, sir::Value right) override {
        return binary("cmp.eq", left, right, sir::Type::I1);
    }
    sir::Value cmpSignedGreaterEqualI32(sir::Value left, sir::Value right) override {
        return binary("cmp.sge", left, right, sir::Type::I1);
    }
    sir::Value cmpUnsignedGreaterEqualI32(sir::Value left, sir::Value right) override {
        return binary("cmp.uge", left, right, sir::Type::I1);
    }
    sir::Value cmpSignedGreaterI32(sir::Value left, sir::Value right) override {
        return binary("cmp.sgt", left, right, sir::Type::I1);
    }
    sir::Value cmpUnsignedGreaterI32(sir::Value left, sir::Value right) override {
        return binary("cmp.ugt", left, right, sir::Type::I1);
    }
    sir::DivideStepResult divideStepI32(sir::Value dividend, sir::Value divisor,
                                        sir::Value q, sir::Value m, sir::Value t) override {
        operations.push_back("div1:" + std::to_string(dividend.id) + ':' +
                             std::to_string(divisor.id) + ':' + std::to_string(q.id) + ':' +
                             std::to_string(m.id) + ':' + std::to_string(t.id));
        return {{nextId_++, sir::Type::I32}, {nextId_++, sir::Type::I1},
                {nextId_++, sir::Type::I1}};
    }
    sir::Value load8(sir::Value address) override {
        return unary("load8", address, sir::Type::I8);
    }
    sir::Value load16(sir::Value address) override {
        return unary("load16", address, sir::Type::I16);
    }
    sir::Value load32(sir::Value address) override { return unary("load32", address); }
    void store8(sir::Value address, sir::Value value) override {
        effect("store8", address, value);
    }
    void store16(sir::Value address, sir::Value value) override {
        effect("store16", address, value);
    }
    void store32(sir::Value address, sir::Value value) override {
        effect("store32", address, value);
    }
    void branch(sir::BasicBlock* target) override {
        operations.push_back("branch:" + pointer(target));
    }
    void branchIf(sir::Value condition, sir::BasicBlock* taken,
                  sir::BasicBlock* notTaken) override {
        operations.push_back("branch-if:" + std::to_string(condition.id) + ':' +
                             pointer(taken) + ':' + pointer(notTaken));
    }
    void branchIndirect(sir::Value target) override {
        operations.push_back("branch-indirect:" + std::to_string(target.id));
    }
    void call(sir::Function* target, sir::BasicBlock* returnSite) override {
        operations.push_back("call:" + pointer(target) + ':' + pointer(returnSite));
    }
    void callIndirect(sir::Value target, sir::BasicBlock* returnSite) override {
        operations.push_back("call-indirect:" + std::to_string(target.id) + ':' +
                             pointer(returnSite));
    }
    void ret(sir::Value target) override {
        operations.push_back("ret:" + std::to_string(target.id));
    }
    void returnFromException() override { operations.push_back("rte"); }
    void trap(std::uint8_t vector) override {
        operations.push_back("trap:" + std::to_string(vector));
    }
    void sleep() override { operations.push_back("sleep"); }

private:
    std::uint32_t nextId_{1000};

    template <typename T>
    static std::string pointer(T* value) {
        return std::to_string(reinterpret_cast<std::uintptr_t>(value));
    }

    sir::Value emit(std::string operation, sir::Type type) {
        operations.push_back(std::move(operation));
        return {nextId_++, type};
    }
    sir::Value unary(const char* name, sir::Value value,
                     sir::Type type = sir::Type::I32) {
        return emit(std::string(name) + ':' + std::to_string(value.id), type);
    }
    sir::Value binary(const char* name, sir::Value left, sir::Value right,
                      sir::Type type = sir::Type::I32) {
        return emit(std::string(name) + ':' + std::to_string(left.id) + ':' +
                        std::to_string(right.id),
                    type);
    }
    void effect(const char* name, sir::Value left, sir::Value right) {
        operations.push_back(std::string(name) + ':' + std::to_string(left.id) + ':' +
                             std::to_string(right.id));
    }
    sir::FlaggedValue flagged(const char* name, sir::Value left, sir::Value right) {
        operations.push_back(std::string(name) + ':' + std::to_string(left.id) + ':' +
                             std::to_string(right.id));
        return {{nextId_++, sir::Type::I32}, {nextId_++, sir::Type::I1}};
    }
    sir::FlaggedValue flagged(const char* name, sir::Value left, sir::Value right,
                              sir::Value flagIn) {
        operations.push_back(std::string(name) + ':' + std::to_string(left.id) + ':' +
                             std::to_string(right.id) + ':' + std::to_string(flagIn.id));
        return {{nextId_++, sir::Type::I32}, {nextId_++, sir::Type::I1}};
    }
};

class RecordingContext final : public sh2::BlockLiftContext {
public:
    RecordingBuilder recordingBuilder;
    std::vector<std::string> writes;

    RecordingContext() {
        for (std::size_t index = 0; index < registers_.size(); ++index) {
            registers_[index] = {static_cast<std::uint32_t>(100 + index), sir::Type::I32};
        }
    }

    sir::Value read(sh2::Register reg) override {
        return registers_[static_cast<std::size_t>(reg)];
    }
    void write(sh2::Register reg, sir::Value value) override {
        registers_[static_cast<std::size_t>(reg)] = value;
        writes.push_back(std::to_string(static_cast<unsigned>(reg)) + ':' +
                         std::to_string(value.id));
    }
    sir::Value readT() override { return t_; }
    void writeT(sir::Value value) override {
        t_ = value;
        writes.push_back("T:" + std::to_string(value.id));
    }
    sir::Value readM() override { return m_; }
    void writeM(sir::Value value) override {
        m_ = value;
        writes.push_back("M:" + std::to_string(value.id));
    }
    sir::Value readQ() override { return q_; }
    void writeQ(sir::Value value) override {
        q_ = value;
        writes.push_back("Q:" + std::to_string(value.id));
    }
    sh2::Address sourcePC() const override { return 0x06010000U; }
    sir::Builder& builder() override { return recordingBuilder; }
    sir::BasicBlock* blockFor(sh2::Address address) override {
        return reinterpret_cast<sir::BasicBlock*>(static_cast<std::uintptr_t>(address));
    }
    sir::Function* functionFor(sh2::Address address) override {
        return reinterpret_cast<sir::Function*>(static_cast<std::uintptr_t>(address));
    }

private:
    std::array<sir::Value, 23> registers_{};
    sir::Value t_{90, sir::Type::I1};
    sir::Value m_{91, sir::Type::I1};
    sir::Value q_{92, sir::Type::I1};
};

void liftsRegisterArithmetic() {
    const sh2::Decoder decoder;
    RecordingContext context;

    const auto mov = decoder.decode(0x06010000U, 0xE3FCU).instruction;
    REQUIRE(sh2::liftInstruction(mov, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations[0] == "const:-4");
    REQUIRE(context.writes[0] == "3:1000");

    const auto add = decoder.decode(0x06010002U, 0x323CU).instruction;
    REQUIRE(sh2::liftInstruction(add, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations.back() == "add:102:1000");
    REQUIRE(context.writes.back() == "2:1001");
}

void liftsSignedLoadAndDisplacedStore() {
    const sh2::Decoder decoder;
    RecordingContext context;

    const auto load = decoder.decode(0x06010000U, 0x6040U).instruction;
    REQUIRE(sh2::liftInstruction(load, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations[0] == "load8:104");
    REQUIRE(context.recordingBuilder.operations[1] == "sext8:1000");
    REQUIRE(context.writes[0] == "0:1001");

    const auto store = decoder.decode(0x06010002U, 0x81EAU).instruction;
    REQUIRE(sh2::liftInstruction(store, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations[2] == "const:20");
    REQUIRE(context.recordingBuilder.operations[3] == "add:114:1002");
    REQUIRE(context.recordingBuilder.operations[4] == "store16:1003:1001");
}

void preservesPostIncrementAliasingRule() {
    const sh2::Decoder decoder;
    RecordingContext context;

    // MOV.L @R4+,R4 must not increment R4 after replacing it with the load.
    const auto load = decoder.decode(0x06010000U, 0x6446U).instruction;
    REQUIRE(sh2::liftInstruction(load, context) == sh2::LiftResult::Success);
    REQUIRE(context.writes.size() == 1);
    REQUIRE(context.writes[0] == "4:1000");
}

void leavesControlFlowForBlockLowering() {
    const sh2::Decoder decoder;
    RecordingContext context;
    const auto branch = decoder.decode(0x06010000U, 0xA000U).instruction;
    REQUIRE(sh2::liftInstruction(branch, context) == sh2::LiftResult::Unsupported);
    REQUIRE(context.recordingBuilder.operations.empty());
    REQUIRE(context.writes.empty());
}

void liftsExtensionsShiftsAndComparisons() {
    const sh2::Decoder decoder;
    RecordingContext context;

    const auto extension = decoder.decode(0x06010000U, 0x611CU).instruction;
    REQUIRE(sh2::liftInstruction(extension, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations[0] == "const:255");
    REQUIRE(context.recordingBuilder.operations[1] == "and:101:1000");
    REQUIRE(context.writes[0] == "1:1001");

    const auto shift = decoder.decode(0x06010002U, 0x4108U).instruction;
    REQUIRE(sh2::liftInstruction(shift, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations[2] == "const:2");
    REQUIRE(context.recordingBuilder.operations[3] == "shl:1001:1002");

    const auto comparison = decoder.decode(0x06010004U, 0x3123U).instruction;
    REQUIRE(sh2::liftInstruction(comparison, context) == sh2::LiftResult::Success);
    REQUIRE(context.recordingBuilder.operations.back() == "cmp.sge:1003:102");
    REQUIRE(context.writes.back() == "T:1004");
}

void liftsCarryAndDivisionState() {
    const sh2::Decoder decoder;

    RecordingContext subtractionContext;
    const auto subc = decoder.decode(0x06010000U, 0x312AU).instruction;
    REQUIRE(sh2::liftInstruction(subc, subtractionContext) == sh2::LiftResult::Success);
    REQUIRE(subtractionContext.recordingBuilder.operations[0] == "subc:101:102:90");
    REQUIRE(subtractionContext.writes[0] == "1:1000");
    REQUIRE(subtractionContext.writes[1] == "T:1001");

    RecordingContext shiftContext;
    const auto shll = decoder.decode(0x06010002U, 0x4100U).instruction;
    REQUIRE(sh2::liftInstruction(shll, shiftContext) == sh2::LiftResult::Success);
    REQUIRE(shiftContext.recordingBuilder.operations[0] == "bit:101:31");
    REQUIRE(shiftContext.recordingBuilder.operations[2] == "shl:101:1001");
    REQUIRE(shiftContext.writes[0] == "1:1002");
    REQUIRE(shiftContext.writes[1] == "T:1000");

    RecordingContext divisionContext;
    const auto div0s = decoder.decode(0x06010004U, 0x2127U).instruction;
    REQUIRE(sh2::liftInstruction(div0s, divisionContext) == sh2::LiftResult::Success);
    REQUIRE(divisionContext.recordingBuilder.operations[0] == "bit:102:31");
    REQUIRE(divisionContext.recordingBuilder.operations[1] == "bit:101:31");
    REQUIRE(divisionContext.recordingBuilder.operations[2] == "xor.i1:1000:1001");
    REQUIRE(divisionContext.writes[0] == "M:1000");
    REQUIRE(divisionContext.writes[1] == "Q:1001");
    REQUIRE(divisionContext.writes[2] == "T:1002");

    const auto div1 = decoder.decode(0x06010006U, 0x3124U).instruction;
    REQUIRE(sh2::liftInstruction(div1, divisionContext) == sh2::LiftResult::Success);
    REQUIRE(divisionContext.recordingBuilder.operations.back() ==
            "div1:101:102:1001:1000:1002");
    REQUIRE(divisionContext.writes[3] == "1:1003");
    REQUIRE(divisionContext.writes[4] == "Q:1004");
    REQUIRE(divisionContext.writes[5] == "T:1005");
}

void lowersDelayedConditionalAfterItsSlot() {
    // BT/S 0x108; ADD #1,R0; RTS; NOP; RTS; NOP.
    const auto image = test::imageFromWords(0x100U, {
        0x8D02U, 0x7001U, 0x000BU, 0x0009U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    const auto* block = cfg.blockAt(0x100U);
    REQUIRE(block != nullptr);

    RecordingContext context;
    const auto result = sh2::lowerBlock(*block, context);
    REQUIRE(result.status == sh2::BlockLiftStatus::Success);
    REQUIRE(context.recordingBuilder.operations[0] == "const:1");
    REQUIRE(context.recordingBuilder.operations[1] == "add:100:1000");
    REQUIRE(context.recordingBuilder.operations[2] == "branch-if:90:264:260");
}

void lowersCallStateBeforeItsDelaySlot() {
    // BSR 0x208; MOV #1,R0; RTS; NOP; RTS; NOP.
    const auto image = test::imageFromWords(0x200U, {
        0xB002U, 0xE001U, 0x000BU, 0x0009U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x200U});
    const auto* block = cfg.blockAt(0x200U);
    REQUIRE(block != nullptr);

    RecordingContext context;
    const auto result = sh2::lowerBlock(*block, context);
    REQUIRE(result.status == sh2::BlockLiftStatus::Success);
    REQUIRE(context.recordingBuilder.operations[0] == "const:516");
    REQUIRE(context.writes[0] == "17:1000");
    REQUIRE(context.recordingBuilder.operations[1] == "const:1");
    REQUIRE(context.writes[1] == "0:1001");
    REQUIRE(context.recordingBuilder.operations[2] == "call:520:516");
}

void snapshotsIndirectTargetsAndReturnAddress() {
    const sh2::Decoder decoder;

    sh2::BasicBlock jumpBlock;
    jumpBlock.start = 0x100U;
    jumpBlock.terminator = sh2::DelayedTerminator{
        decoder.decode(0x100U, 0x432BU).instruction,
        decoder.decode(0x102U, 0xE301U).instruction,
    };
    RecordingContext jumpContext;
    REQUIRE(sh2::lowerBlock(jumpBlock, jumpContext));
    REQUIRE(jumpContext.writes[0] == "3:1000");
    REQUIRE(jumpContext.recordingBuilder.operations.back() == "branch-indirect:103");

    sh2::BasicBlock returnBlock;
    returnBlock.start = 0x200U;
    returnBlock.terminator = sh2::DelayedTerminator{
        decoder.decode(0x200U, 0x000BU).instruction,
        decoder.decode(0x202U, 0x432AU).instruction,
    };
    RecordingContext returnContext;
    REQUIRE(sh2::lowerBlock(returnBlock, returnContext));
    REQUIRE(returnContext.writes[0] == "17:1000");
    REQUIRE(returnContext.recordingBuilder.operations.back() == "ret:117");
}

void rejectsUnsafeDelaySlotBeforeEmission() {
    const sh2::Decoder decoder;
    sh2::BasicBlock block;
    block.start = 0x100U;
    block.terminator = sh2::DelayedTerminator{
        decoder.decode(0x100U, 0xA002U).instruction,
        decoder.decode(0x102U, 0x000BU).instruction,
    };

    RecordingContext context;
    const auto result = sh2::lowerBlock(block, context);
    REQUIRE(result.status == sh2::BlockLiftStatus::InvalidDelaySlot);
    REQUIRE(result.address == 0x102U);
    REQUIRE(context.recordingBuilder.operations.empty());
    REQUIRE(context.writes.empty());
}

void lowersRemainingControlFlowForms() {
    const sh2::Decoder decoder;

    sh2::BasicBlock directBranch;
    directBranch.start = 0x100U;
    directBranch.terminator = sh2::DelayedTerminator{
        decoder.decode(0x100U, 0xA002U).instruction,
        decoder.decode(0x102U, 0x0009U).instruction,
    };
    RecordingContext branchContext;
    REQUIRE(sh2::lowerBlock(directBranch, branchContext));
    REQUIRE(branchContext.recordingBuilder.operations.back() == "branch:264");

    sh2::BasicBlock indirectCall;
    indirectCall.start = 0x200U;
    indirectCall.terminator = sh2::DelayedTerminator{
        decoder.decode(0x200U, 0x430BU).instruction,
        decoder.decode(0x202U, 0x0009U).instruction,
    };
    RecordingContext callContext;
    REQUIRE(sh2::lowerBlock(indirectCall, callContext));
    REQUIRE(callContext.writes[0] == "17:1000");
    REQUIRE(callContext.recordingBuilder.operations.back() == "call-indirect:103:516");

    sh2::BasicBlock exceptionReturn;
    exceptionReturn.start = 0x300U;
    exceptionReturn.terminator = sh2::DelayedTerminator{
        decoder.decode(0x300U, 0x002BU).instruction,
        decoder.decode(0x302U, 0x0009U).instruction,
    };
    RecordingContext rteContext;
    REQUIRE(sh2::lowerBlock(exceptionReturn, rteContext));
    REQUIRE(rteContext.recordingBuilder.operations.back() == "rte");

    sh2::BasicBlock trap;
    trap.start = 0x400U;
    trap.terminator = sh2::DelayedTerminator{
        decoder.decode(0x400U, 0xC33FU).instruction,
        std::nullopt,
    };
    RecordingContext trapContext;
    REQUIRE(sh2::lowerBlock(trap, trapContext));
    REQUIRE(trapContext.recordingBuilder.operations.back() == "trap:63");

    sh2::BasicBlock fallthrough;
    fallthrough.start = 0x500U;
    fallthrough.instructions.push_back(decoder.decode(0x500U, 0x001BU).instruction);
    fallthrough.successors.push_back({0x502U, sh2::EdgeKind::Fallthrough});
    RecordingContext fallthroughContext;
    REQUIRE(sh2::lowerBlock(fallthrough, fallthroughContext));
    REQUIRE(fallthroughContext.recordingBuilder.operations[0] == "sleep");
    REQUIRE(fallthroughContext.recordingBuilder.operations[1] == "branch:1282");
}

} // namespace

void registerLifterTests(test::Registry& registry) {
    registry.add("lifter: register arithmetic", liftsRegisterArithmetic);
    registry.add("lifter: signed load and displaced store", liftsSignedLoadAndDisplacedStore);
    registry.add("lifter: post-increment aliasing", preservesPostIncrementAliasingRule);
    registry.add("lifter: control flow deferred to block lowering", leavesControlFlowForBlockLowering);
    registry.add("lifter: extensions, shifts and comparisons",
                 liftsExtensionsShiftsAndComparisons);
    registry.add("lifter: carry and division state", liftsCarryAndDivisionState);
    registry.add("block lifter: delayed conditional ordering",
                 lowersDelayedConditionalAfterItsSlot);
    registry.add("block lifter: call state and delay slot ordering",
                 lowersCallStateBeforeItsDelaySlot);
    registry.add("block lifter: snapshots dynamic transfer state",
                 snapshotsIndirectTargetsAndReturnAddress);
    registry.add("block lifter: rejects unsafe delay slots",
                 rejectsUnsafeDelaySlotBeforeEmission);
    registry.add("block lifter: remaining control-flow forms",
                 lowersRemainingControlFlowForms);
}
