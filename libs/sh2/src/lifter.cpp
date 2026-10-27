#include "saturn/sh2/lifter.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace saturn::sh2 {
namespace {

[[nodiscard]] Register reg(const DecodedInstruction& instruction, std::size_t index) {
    return std::get<RegisterOperand>(instruction.operand(index)).reg;
}

[[nodiscard]] std::int32_t immediate(const DecodedInstruction& instruction,
                                     std::size_t index = 0) {
    return std::get<ImmediateOperand>(instruction.operand(index)).value;
}

[[nodiscard]] sir::Value addOffset(LiftContext& context, sir::Value base,
                                   std::int32_t offset) {
    if (offset == 0) {
        return base;
    }
    return context.builder().addI32(base, context.builder().constI32(offset));
}

[[nodiscard]] sir::Value addressOf(LiftContext& context, const MemoryOperand& memory,
                                   sir::Value base) {
    auto address = base;
    if (memory.index) {
        address = context.builder().addI32(address, context.read(*memory.index));
    }
    return addOffset(context, address, memory.displacement);
}

[[nodiscard]] sir::Value loadMemory(LiftContext& context, sir::Value address,
                                    std::uint8_t width) {
    switch (width) {
    case 1:
        return context.builder().signExtendI8ToI32(context.builder().load8(address));
    case 2:
        return context.builder().signExtendI16ToI32(context.builder().load16(address));
    case 4:
        return context.builder().load32(address);
    default:
        return {};
    }
}

void storeMemory(LiftContext& context, sir::Value address, sir::Value value,
                 std::uint8_t width) {
    switch (width) {
    case 1: context.builder().store8(address, value); break;
    case 2: context.builder().store16(address, value); break;
    case 4: context.builder().store32(address, value); break;
    default: break;
    }
}

LiftResult liftMovReg(const DecodedInstruction& instruction, LiftContext& context) {
    context.write(reg(instruction, 1),
                  context.builder().copyI32(context.read(reg(instruction, 0))));
    return LiftResult::Success;
}

LiftResult liftMovImm(const DecodedInstruction& instruction, LiftContext& context) {
    context.write(reg(instruction, 1), context.builder().constI32(immediate(instruction)));
    return LiftResult::Success;
}

LiftResult liftMovT(const DecodedInstruction& instruction, LiftContext& context) {
    context.write(reg(instruction, 0),
                  context.builder().zeroExtendI1ToI32(context.readT()));
    return LiftResult::Success;
}

LiftResult liftMova(const DecodedInstruction& instruction, LiftContext& context) {
    const auto address = std::get<AddressOperand>(instruction.operand(0)).address;
    context.write(reg(instruction, 1),
                  context.builder().constI32(static_cast<std::int32_t>(address)));
    return LiftResult::Success;
}

LiftResult liftLoad(const DecodedInstruction& instruction, LiftContext& context) {
    const auto& memory = std::get<MemoryOperand>(instruction.operand(0));
    const auto destination = reg(instruction, 1);
    const auto base = context.read(memory.base);
    const auto address = addressOf(context, memory, base);
    const auto value = loadMemory(context, address, memory.width);
    if (!value) {
        return LiftResult::Unsupported;
    }
    context.write(destination, value);
    if (memory.mode == MemoryOperand::Mode::PostIncrement && memory.base != destination) {
        context.write(memory.base,
                      addOffset(context, base, static_cast<std::int32_t>(memory.width)));
    }
    return LiftResult::Success;
}

LiftResult liftPcRelativeLoad(const DecodedInstruction& instruction, LiftContext& context) {
    const auto address = std::get<AddressOperand>(instruction.operand(0)).address;
    const auto destination = reg(instruction, 1);
    const auto pointer = context.builder().constI32(static_cast<std::int32_t>(address));
    const auto width = instruction.opcode == Opcode::MovWordPcRelative ? 2U : 4U;
    const auto value = loadMemory(context, pointer, static_cast<std::uint8_t>(width));
    context.write(destination, value);
    return LiftResult::Success;
}

LiftResult liftStore(const DecodedInstruction& instruction, LiftContext& context) {
    const auto source = reg(instruction, 0);
    const auto& memory = std::get<MemoryOperand>(instruction.operand(1));
    auto base = context.read(memory.base);
    if (memory.mode == MemoryOperand::Mode::PreDecrement) {
        base = addOffset(context, base, -static_cast<std::int32_t>(memory.width));
        context.write(memory.base, base);
    }
    storeMemory(context, addressOf(context, memory, base), context.read(source), memory.width);
    return LiftResult::Success;
}

LiftResult liftStsPreDecrement(const DecodedInstruction& instruction, LiftContext& context) {
    return liftStore(instruction, context);
}

LiftResult liftLdsPostIncrement(const DecodedInstruction& instruction, LiftContext& context) {
    return liftLoad(instruction, context);
}

LiftResult liftSpecialCopy(const DecodedInstruction& instruction, LiftContext& context) {
    return liftMovReg(instruction, context);
}

LiftResult liftAdd(const DecodedInstruction& instruction, LiftContext& context) {
    const auto source = reg(instruction, 0);
    const auto destination = reg(instruction, 1);
    context.write(destination,
                  context.builder().addI32(context.read(destination), context.read(source)));
    return LiftResult::Success;
}

LiftResult liftAddImm(const DecodedInstruction& instruction, LiftContext& context) {
    const auto destination = reg(instruction, 1);
    context.write(destination,
                  context.builder().addI32(context.read(destination),
                                           context.builder().constI32(immediate(instruction))));
    return LiftResult::Success;
}

LiftResult liftSub(const DecodedInstruction& instruction, LiftContext& context) {
    const auto source = reg(instruction, 0);
    const auto destination = reg(instruction, 1);
    context.write(destination,
                  context.builder().subI32(context.read(destination), context.read(source)));
    return LiftResult::Success;
}

LiftResult liftNeg(const DecodedInstruction& instruction, LiftContext& context) {
    context.write(reg(instruction, 1),
                  context.builder().subI32(context.builder().constI32(0),
                                           context.read(reg(instruction, 0))));
    return LiftResult::Success;
}

LiftResult liftMulLong(const DecodedInstruction& instruction, LiftContext& context) {
    context.write(Register::MACL,
                  context.builder().mulI32(context.read(reg(instruction, 1)),
                                           context.read(reg(instruction, 0))));
    return LiftResult::Success;
}

LiftResult liftMulWord(const DecodedInstruction& instruction, LiftContext& context) {
    const auto source = context.read(reg(instruction, 0));
    const auto destination = context.read(reg(instruction, 1));
    const auto sourceWord = context.builder().truncateI32ToI16(source);
    const auto destinationWord = context.builder().truncateI32ToI16(destination);
    sir::Value left = sourceWord;
    sir::Value right = destinationWord;
    if (instruction.opcode == Opcode::MulSignedWord) {
        left = context.builder().signExtendI16ToI32(sourceWord);
        right = context.builder().signExtendI16ToI32(destinationWord);
    } else {
        left = context.builder().andI32(source, context.builder().constI32(0xFFFFU));
        right = context.builder().andI32(destination, context.builder().constI32(0xFFFFU));
    }
    context.write(Register::MACL, context.builder().mulI32(right, left));
    return LiftResult::Success;
}

LiftResult liftExtension(const DecodedInstruction& instruction, LiftContext& context) {
    const auto source = context.read(reg(instruction, 0));
    sir::Value result;
    switch (instruction.opcode) {
    case Opcode::ExtUnsignedByte:
        result = context.builder().andI32(source, context.builder().constI32(0xFF));
        break;
    case Opcode::ExtUnsignedWord:
        result = context.builder().andI32(source, context.builder().constI32(0xFFFF));
        break;
    case Opcode::ExtSignedByte:
        result = context.builder().signExtendI8ToI32(
            context.builder().truncateI32ToI8(source));
        break;
    case Opcode::ExtSignedWord:
        result = context.builder().signExtendI16ToI32(
            context.builder().truncateI32ToI16(source));
        break;
    default:
        return LiftResult::Unsupported;
    }
    context.write(reg(instruction, 1), result);
    return LiftResult::Success;
}

LiftResult liftFixedShift(const DecodedInstruction& instruction, LiftContext& context) {
    const auto destination = reg(instruction, 0);
    std::int32_t amount{};
    bool left{};
    switch (instruction.opcode) {
    case Opcode::Shll2: amount = 2; left = true; break;
    case Opcode::Shll8: amount = 8; left = true; break;
    case Opcode::Shll16: amount = 16; left = true; break;
    case Opcode::Shlr2: amount = 2; break;
    case Opcode::Shlr8: amount = 8; break;
    case Opcode::Shlr16: amount = 16; break;
    default: return LiftResult::Unsupported;
    }
    const auto shift = context.builder().constI32(amount);
    const auto value = context.read(destination);
    context.write(destination,
                  left ? context.builder().shiftLeftI32(value, shift)
                       : context.builder().logicalShiftRightI32(value, shift));
    return LiftResult::Success;
}

LiftResult liftFlaggedArithmetic(const DecodedInstruction& instruction,
                                 LiftContext& context) {
    auto& builder = context.builder();
    const auto source = context.read(reg(instruction, 0));
    const auto destination = reg(instruction, 1);
    const auto current = context.read(destination);
    sir::FlaggedValue result;
    switch (instruction.opcode) {
    case Opcode::AddCarry:
        result = builder.addWithCarryI32(current, source, context.readT());
        break;
    case Opcode::SubCarry:
        result = builder.subWithBorrowI32(current, source, context.readT());
        break;
    case Opcode::NegCarry:
        result = builder.subWithBorrowI32(builder.constI32(0), source, context.readT());
        break;
    case Opcode::AddOverflow:
        result = builder.addWithOverflowI32(current, source);
        break;
    case Opcode::SubOverflow:
        result = builder.subWithOverflowI32(current, source);
        break;
    default:
        return LiftResult::Unsupported;
    }
    context.write(destination, result.value);
    context.writeT(result.flag);
    return LiftResult::Success;
}

LiftResult liftShiftWithT(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto destination = reg(instruction, 0);
    const auto value = context.read(destination);
    const auto left = instruction.opcode == Opcode::Shll || instruction.opcode == Opcode::Shal;
    const auto outgoing = builder.extractBitI32(value, left ? 31U : 0U);
    const auto one = builder.constI32(1);
    sir::Value shifted;
    if (left) {
        shifted = builder.shiftLeftI32(value, one);
    } else if (instruction.opcode == Opcode::Shar) {
        shifted = builder.arithmeticShiftRightI32(value, one);
    } else {
        shifted = builder.logicalShiftRightI32(value, one);
    }
    context.write(destination, shifted);
    context.writeT(outgoing);
    return LiftResult::Success;
}

LiftResult liftRotate(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto destination = reg(instruction, 0);
    const auto value = context.read(destination);
    const auto left = instruction.opcode == Opcode::Rotl || instruction.opcode == Opcode::Rotcl;
    const auto throughCarry = instruction.opcode == Opcode::Rotcl ||
                              instruction.opcode == Opcode::Rotcr;
    const auto outgoing = builder.extractBitI32(value, left ? 31U : 0U);
    const auto incoming = throughCarry ? context.readT() : outgoing;
    const auto one = builder.constI32(1);
    sir::Value rotated;
    if (left) {
        const auto shifted = builder.shiftLeftI32(value, one);
        rotated = builder.orI32(shifted, builder.zeroExtendI1ToI32(incoming));
    } else {
        const auto shifted = builder.logicalShiftRightI32(value, one);
        const auto highBit = builder.shiftLeftI32(
            builder.zeroExtendI1ToI32(incoming), builder.constI32(31));
        rotated = builder.orI32(shifted, highBit);
    }
    context.write(destination, rotated);
    context.writeT(outgoing);
    return LiftResult::Success;
}

LiftResult liftDt(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto destination = reg(instruction, 0);
    const auto result = builder.subI32(context.read(destination), builder.constI32(1));
    context.write(destination, result);
    context.writeT(builder.cmpEqI32(result, builder.constI32(0)));
    return LiftResult::Success;
}

LiftResult liftDiv0Signed(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto m = builder.extractBitI32(context.read(reg(instruction, 0)), 31);
    const auto q = builder.extractBitI32(context.read(reg(instruction, 1)), 31);
    context.writeM(m);
    context.writeQ(q);
    context.writeT(builder.xorI1(m, q));
    return LiftResult::Success;
}

LiftResult liftDiv0Unsigned(const DecodedInstruction&, LiftContext& context) {
    const auto zero = context.builder().constI1(false);
    context.writeM(zero);
    context.writeQ(zero);
    context.writeT(zero);
    return LiftResult::Success;
}

LiftResult liftDiv1(const DecodedInstruction& instruction, LiftContext& context) {
    const auto destination = reg(instruction, 1);
    const auto result = context.builder().divideStepI32(
        context.read(destination), context.read(reg(instruction, 0)),
        context.readQ(), context.readM(), context.readT());
    context.write(destination, result.value);
    context.writeQ(result.q);
    context.writeT(result.t);
    return LiftResult::Success;
}

template <sir::Value (sir::Builder::*Operation)(sir::Value, sir::Value)>
LiftResult liftBinary(const DecodedInstruction& instruction, LiftContext& context) {
    const auto source = reg(instruction, 0);
    const auto destination = reg(instruction, 1);
    auto& builder = context.builder();
    context.write(destination, (builder.*Operation)(context.read(destination),
                                                    context.read(source)));
    return LiftResult::Success;
}

template <sir::Value (sir::Builder::*Operation)(sir::Value, sir::Value)>
LiftResult liftImmediateBinary(const DecodedInstruction& instruction, LiftContext& context) {
    const auto destination = reg(instruction, 1);
    auto& builder = context.builder();
    context.write(destination, (builder.*Operation)(
        context.read(destination), builder.constI32(immediate(instruction))));
    return LiftResult::Success;
}

LiftResult liftNot(const DecodedInstruction& instruction, LiftContext& context) {
    context.write(reg(instruction, 1),
                  context.builder().notI32(context.read(reg(instruction, 0))));
    return LiftResult::Success;
}

LiftResult liftCmpEq(const DecodedInstruction& instruction, LiftContext& context) {
    context.writeT(context.builder().cmpEqI32(context.read(reg(instruction, 1)),
                                              context.read(reg(instruction, 0))));
    return LiftResult::Success;
}

LiftResult liftCmpStr(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto difference = builder.xorI32(context.read(reg(instruction, 1)),
                                           context.read(reg(instruction, 0)));
    auto anyEqual = builder.constI32(0);
    for (std::uint8_t shift = 0; shift < 32; shift += 8) {
        const auto byte = builder.andI32(
            builder.logicalShiftRightI32(difference, builder.constI32(shift)),
            builder.constI32(0xFF));
        const auto equal = builder.cmpEqI32(byte, builder.constI32(0));
        anyEqual = builder.orI32(anyEqual, builder.zeroExtendI1ToI32(equal));
    }
    context.writeT(builder.cmpUnsignedGreaterI32(anyEqual, builder.constI32(0)));
    return LiftResult::Success;
}

LiftResult liftCmpEqImm(const DecodedInstruction& instruction, LiftContext& context) {
    context.writeT(context.builder().cmpEqI32(
        context.read(reg(instruction, 1)), context.builder().constI32(immediate(instruction))));
    return LiftResult::Success;
}

LiftResult liftComparison(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto right = context.read(reg(instruction, 0));
    const auto left = context.read(reg(instruction, 1));
    sir::Value result;
    switch (instruction.opcode) {
    case Opcode::CmpHs: result = builder.cmpUnsignedGreaterEqualI32(left, right); break;
    case Opcode::CmpGe: result = builder.cmpSignedGreaterEqualI32(left, right); break;
    case Opcode::CmpHi: result = builder.cmpUnsignedGreaterI32(left, right); break;
    case Opcode::CmpGt: result = builder.cmpSignedGreaterI32(left, right); break;
    default: return LiftResult::Unsupported;
    }
    context.writeT(result);
    return LiftResult::Success;
}

LiftResult liftComparisonZero(const DecodedInstruction& instruction, LiftContext& context) {
    auto& builder = context.builder();
    const auto value = context.read(reg(instruction, 0));
    const auto zero = builder.constI32(0);
    context.writeT(instruction.opcode == Opcode::CmpPz
                       ? builder.cmpSignedGreaterEqualI32(value, zero)
                       : builder.cmpSignedGreaterI32(value, zero));
    return LiftResult::Success;
}

LiftResult liftTst(const DecodedInstruction& instruction, LiftContext& context) {
    const auto value = context.builder().andI32(context.read(reg(instruction, 1)),
                                                context.read(reg(instruction, 0)));
    context.writeT(context.builder().cmpEqI32(value, context.builder().constI32(0)));
    return LiftResult::Success;
}

LiftResult liftTstImm(const DecodedInstruction& instruction, LiftContext& context) {
    const auto value = context.builder().andI32(
        context.read(reg(instruction, 1)), context.builder().constI32(immediate(instruction)));
    context.writeT(context.builder().cmpEqI32(value, context.builder().constI32(0)));
    return LiftResult::Success;
}

LiftResult liftNop(const DecodedInstruction&, LiftContext&) {
    return LiftResult::Success;
}

LiftResult liftSleep(const DecodedInstruction&, LiftContext& context) {
    context.builder().sleep();
    return LiftResult::Success;
}

constexpr auto kLifters = std::to_array<std::pair<Opcode, LiftFn>>({
    {Opcode::MovReg, liftMovReg}, {Opcode::MovImm, liftMovImm},
    {Opcode::MovT, liftMovT}, {Opcode::Mova, liftMova},
    {Opcode::MovByteLoad, liftLoad}, {Opcode::MovWordLoad, liftLoad},
    {Opcode::MovLongLoad, liftLoad}, {Opcode::MovBytePostIncrement, liftLoad},
    {Opcode::MovWordPostIncrement, liftLoad}, {Opcode::MovLongPostIncrement, liftLoad},
    {Opcode::MovByteIndexedLoad, liftLoad}, {Opcode::MovWordIndexedLoad, liftLoad},
    {Opcode::MovLongIndexedLoad, liftLoad}, {Opcode::MovByteDisplacementLoad, liftLoad},
    {Opcode::MovWordDisplacementLoad, liftLoad}, {Opcode::MovLongDisplacementLoad, liftLoad},
    {Opcode::MovWordPcRelative, liftPcRelativeLoad},
    {Opcode::MovLongPcRelative, liftPcRelativeLoad},
    {Opcode::MovByteStore, liftStore}, {Opcode::MovWordStore, liftStore},
    {Opcode::MovLongStore, liftStore}, {Opcode::MovBytePreDecrement, liftStore},
    {Opcode::MovWordPreDecrement, liftStore}, {Opcode::MovLongPreDecrement, liftStore},
    {Opcode::MovByteIndexedStore, liftStore}, {Opcode::MovWordIndexedStore, liftStore},
    {Opcode::MovLongIndexedStore, liftStore}, {Opcode::MovByteDisplacementStore, liftStore},
    {Opcode::MovWordDisplacementStore, liftStore}, {Opcode::MovLongDisplacementStore, liftStore},
    {Opcode::StsPrPreDecrement, liftStsPreDecrement},
    {Opcode::LdsPrPostIncrement, liftLdsPostIncrement},
    {Opcode::Stc, liftSpecialCopy}, {Opcode::Sts, liftSpecialCopy},
    {Opcode::Ldc, liftSpecialCopy}, {Opcode::Lds, liftSpecialCopy},
    {Opcode::Add, liftAdd}, {Opcode::AddImm, liftAddImm}, {Opcode::Sub, liftSub},
    {Opcode::Neg, liftNeg}, {Opcode::MulLong, liftMulLong},
    {Opcode::MulSignedWord, liftMulWord}, {Opcode::MulUnsignedWord, liftMulWord},
    {Opcode::AddCarry, liftFlaggedArithmetic},
    {Opcode::AddOverflow, liftFlaggedArithmetic},
    {Opcode::SubCarry, liftFlaggedArithmetic},
    {Opcode::SubOverflow, liftFlaggedArithmetic},
    {Opcode::NegCarry, liftFlaggedArithmetic},
    {Opcode::ExtUnsignedByte, liftExtension}, {Opcode::ExtUnsignedWord, liftExtension},
    {Opcode::ExtSignedByte, liftExtension}, {Opcode::ExtSignedWord, liftExtension},
    {Opcode::Shll2, liftFixedShift}, {Opcode::Shll8, liftFixedShift},
    {Opcode::Shll16, liftFixedShift}, {Opcode::Shlr2, liftFixedShift},
    {Opcode::Shlr8, liftFixedShift}, {Opcode::Shlr16, liftFixedShift},
    {Opcode::Shll, liftShiftWithT}, {Opcode::Shal, liftShiftWithT},
    {Opcode::Shlr, liftShiftWithT}, {Opcode::Shar, liftShiftWithT},
    {Opcode::Rotl, liftRotate}, {Opcode::Rotr, liftRotate},
    {Opcode::Rotcl, liftRotate}, {Opcode::Rotcr, liftRotate},
    {Opcode::Dt, liftDt}, {Opcode::Div0Signed, liftDiv0Signed},
    {Opcode::Div0Unsigned, liftDiv0Unsigned}, {Opcode::Div1, liftDiv1},
    {Opcode::And, liftBinary<&sir::Builder::andI32>},
    {Opcode::Or, liftBinary<&sir::Builder::orI32>},
    {Opcode::Xor, liftBinary<&sir::Builder::xorI32>},
    {Opcode::AndImm, liftImmediateBinary<&sir::Builder::andI32>},
    {Opcode::OrImm, liftImmediateBinary<&sir::Builder::orI32>},
    {Opcode::XorImm, liftImmediateBinary<&sir::Builder::xorI32>},
    {Opcode::Not, liftNot}, {Opcode::CmpEq, liftCmpEq},
    {Opcode::CmpEqImm, liftCmpEqImm}, {Opcode::Tst, liftTst},
    {Opcode::CmpHs, liftComparison}, {Opcode::CmpGe, liftComparison},
    {Opcode::CmpHi, liftComparison}, {Opcode::CmpGt, liftComparison},
    {Opcode::CmpPz, liftComparisonZero}, {Opcode::CmpPl, liftComparisonZero},
    {Opcode::TstImm, liftTstImm},
    {Opcode::CmpStr, liftCmpStr},
    {Opcode::Nop, liftNop}, {Opcode::Sleep, liftSleep},
});

} // namespace

LiftFn lifterFor(Opcode opcode) noexcept {
    for (const auto& [candidate, function] : kLifters) {
        if (candidate == opcode) {
            return function;
        }
    }
    return nullptr;
}

LiftResult liftInstruction(const DecodedInstruction& instruction, LiftContext& context) {
    const auto function = lifterFor(instruction.opcode);
    return function ? function(instruction, context) : LiftResult::Unsupported;
}

} // namespace saturn::sh2
