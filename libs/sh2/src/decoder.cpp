#include "saturn/sh2/decoder.hpp"

#include <stdexcept>
#include <utility>

namespace saturn::sh2 {
namespace {

enum class Encoding : std::uint8_t {
    Fixed,
    RegReg,
    RegImm8,
    MemoryLoad,
    MemoryStore,
    IndexedMemoryLoad,
    IndexedMemoryStore,
    DisplacementMemoryLoad,
    DisplacementMemoryStore,
    DisplacementR0Load,
    DisplacementR0Store,
    RegOnly,
    Disp8,
    Disp12,
    PcWordDisp8,
    PcWordLoadDisp8,
    PcLongLoadDisp8,
    IndirectReg,
    CompareImmR0,
    UnsignedImmR0,
    UnsignedImm,
    SrStore,
    GbrStore,
    VbrStore,
    MachStore,
    MaclStore,
    PrStore,
    SrLoad,
    GbrLoad,
    VbrLoad,
    MachLoad,
    MaclLoad,
    PrLoad,
    StsMachPreDecrement,
    StsMaclPreDecrement,
    StsPrPreDecrement,
    LdsMachPostIncrement,
    LdsMaclPostIncrement,
    LdsPrPostIncrement,
};

struct InstructionPattern {
    std::uint16_t mask;
    std::uint16_t value;
    Opcode opcode;
    Encoding encoding;
    FlowKind flow;
    DelaySlot delaySlot;
    std::uint8_t memoryWidth{};
};

constexpr InstructionPattern patterns[] = {
    {0xFFFF, 0x0009, Opcode::Nop,       Encoding::Fixed,        FlowKind::Fallthrough,       DelaySlot::None},
    {0xFFFF, 0x000B, Opcode::Rts,       Encoding::Fixed,        FlowKind::Return,            DelaySlot::Always},
    {0xFFFF, 0x001B, Opcode::Sleep,     Encoding::Fixed,        FlowKind::Fallthrough,       DelaySlot::None},
    {0xFFFF, 0x0019, Opcode::Div0Unsigned, Encoding::Fixed,     FlowKind::Fallthrough,       DelaySlot::None},
    {0xFFFF, 0x002B, Opcode::Rte,       Encoding::Fixed,        FlowKind::ExceptionReturn,   DelaySlot::Always},

    {0xF0FF, 0x0003, Opcode::Bsrf,      Encoding::IndirectReg,  FlowKind::IndirectCall,      DelaySlot::Always},
    {0xF0FF, 0x0023, Opcode::Braf,      Encoding::IndirectReg,  FlowKind::IndirectBranch,    DelaySlot::Always},
    {0xF0FF, 0x0002, Opcode::Stc,       Encoding::SrStore,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x0012, Opcode::Stc,       Encoding::GbrStore,     FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x0022, Opcode::Stc,       Encoding::VbrStore,     FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x000A, Opcode::Sts,       Encoding::MachStore,    FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x001A, Opcode::Sts,       Encoding::MaclStore,    FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x002A, Opcode::Sts,       Encoding::PrStore,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x400E, Opcode::Ldc,       Encoding::SrLoad,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x401E, Opcode::Ldc,       Encoding::GbrLoad,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x402E, Opcode::Ldc,       Encoding::VbrLoad,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x400A, Opcode::Lds,       Encoding::MachLoad,     FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x401A, Opcode::Lds,       Encoding::MaclLoad,     FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x402A, Opcode::Lds,       Encoding::PrLoad,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x400B, Opcode::Jsr,       Encoding::IndirectReg,  FlowKind::IndirectCall,      DelaySlot::Always},
    {0xF0FF, 0x402B, Opcode::Jmp,       Encoding::IndirectReg,  FlowKind::IndirectBranch,    DelaySlot::Always},

    {0xF0FF, 0x0029, Opcode::MovT,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4011, Opcode::CmpPz,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4015, Opcode::CmpPl,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4000, Opcode::Shll,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4008, Opcode::Shll2,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4018, Opcode::Shll8,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4028, Opcode::Shll16,    Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4001, Opcode::Shlr,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4009, Opcode::Shlr2,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4019, Opcode::Shlr8,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4029, Opcode::Shlr16,    Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4020, Opcode::Shal,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4021, Opcode::Shar,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4004, Opcode::Rotl,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4005, Opcode::Rotr,      Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4024, Opcode::Rotcl,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4025, Opcode::Rotcr,     Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4010, Opcode::Dt,        Encoding::RegOnly,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF0FF, 0x4002, Opcode::StsPrPreDecrement, Encoding::StsMachPreDecrement, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF0FF, 0x4012, Opcode::StsPrPreDecrement, Encoding::StsMaclPreDecrement, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF0FF, 0x4022, Opcode::StsPrPreDecrement, Encoding::StsPrPreDecrement, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF0FF, 0x4006, Opcode::LdsPrPostIncrement, Encoding::LdsMachPostIncrement, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF0FF, 0x4016, Opcode::LdsPrPostIncrement, Encoding::LdsMaclPostIncrement, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF0FF, 0x4026, Opcode::LdsPrPostIncrement, Encoding::LdsPrPostIncrement, FlowKind::Fallthrough, DelaySlot::None, 4},

    {0xF00F, 0x2000, Opcode::MovByteStore, Encoding::MemoryStore, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xF00F, 0x2001, Opcode::MovWordStore, Encoding::MemoryStore, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xF00F, 0x2002, Opcode::MovLongStore, Encoding::MemoryStore, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF00F, 0x2004, Opcode::MovBytePreDecrement, Encoding::MemoryStore, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xF00F, 0x2005, Opcode::MovWordPreDecrement, Encoding::MemoryStore, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xF00F, 0x2006, Opcode::MovLongPreDecrement, Encoding::MemoryStore, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF00F, 0x2008, Opcode::Tst,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x2009, Opcode::And,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x200A, Opcode::Xor,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x200B, Opcode::Or,        Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x2007, Opcode::Div0Signed, Encoding::RegReg,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x200E, Opcode::MulUnsignedWord, Encoding::RegReg, FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x200F, Opcode::MulSignedWord, Encoding::RegReg,   FlowKind::Fallthrough,       DelaySlot::None},

    {0xF00F, 0x300A, Opcode::SubCarry,  Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x300B, Opcode::SubOverflow, Encoding::RegReg,     FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3000, Opcode::CmpEq,     Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3002, Opcode::CmpHs,     Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3003, Opcode::CmpGe,     Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3004, Opcode::Div1,      Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3006, Opcode::CmpHi,     Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3007, Opcode::CmpGt,     Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x3008, Opcode::Sub,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x300C, Opcode::Add,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x300E, Opcode::AddCarry,  Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x300F, Opcode::AddOverflow, Encoding::RegReg,     FlowKind::Fallthrough,       DelaySlot::None},

    {0xF00F, 0x200C, Opcode::CmpStr,     Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},

    {0xF00F, 0x0007, Opcode::MulLong,   Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x0004, Opcode::MovByteIndexedStore, Encoding::IndexedMemoryStore, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xF00F, 0x0005, Opcode::MovWordIndexedStore, Encoding::IndexedMemoryStore, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xF00F, 0x0006, Opcode::MovLongIndexedStore, Encoding::IndexedMemoryStore, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF00F, 0x000C, Opcode::MovByteIndexedLoad, Encoding::IndexedMemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xF00F, 0x000D, Opcode::MovWordIndexedLoad, Encoding::IndexedMemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xF00F, 0x000E, Opcode::MovLongIndexedLoad, Encoding::IndexedMemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF000, 0x1000, Opcode::MovLongDisplacementStore, Encoding::DisplacementMemoryStore, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF000, 0x5000, Opcode::MovLongDisplacementLoad, Encoding::DisplacementMemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF00F, 0x6000, Opcode::MovByteLoad, Encoding::MemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xF00F, 0x6001, Opcode::MovWordLoad, Encoding::MemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xF00F, 0x6002, Opcode::MovLongLoad, Encoding::MemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF00F, 0x6004, Opcode::MovBytePostIncrement, Encoding::MemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xF00F, 0x6005, Opcode::MovWordPostIncrement, Encoding::MemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xF00F, 0x6006, Opcode::MovLongPostIncrement, Encoding::MemoryLoad, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xF00F, 0x6003, Opcode::MovReg,    Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x6007, Opcode::Not,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x600A, Opcode::NegCarry,  Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x600B, Opcode::Neg,       Encoding::RegReg,       FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x600C, Opcode::ExtUnsignedByte, Encoding::RegReg, FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x600D, Opcode::ExtUnsignedWord, Encoding::RegReg, FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x600E, Opcode::ExtSignedByte, Encoding::RegReg,   FlowKind::Fallthrough,       DelaySlot::None},
    {0xF00F, 0x600F, Opcode::ExtSignedWord, Encoding::RegReg,   FlowKind::Fallthrough,       DelaySlot::None},

    {0xF000, 0x7000, Opcode::AddImm,    Encoding::RegImm8,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xFF00, 0x8000, Opcode::MovByteDisplacementStore, Encoding::DisplacementR0Store, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xFF00, 0x8100, Opcode::MovWordDisplacementStore, Encoding::DisplacementR0Store, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xFF00, 0x8400, Opcode::MovByteDisplacementLoad, Encoding::DisplacementR0Load, FlowKind::Fallthrough, DelaySlot::None, 1},
    {0xFF00, 0x8500, Opcode::MovWordDisplacementLoad, Encoding::DisplacementR0Load, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xFF00, 0x8800, Opcode::CmpEqImm,  Encoding::CompareImmR0, FlowKind::Fallthrough,       DelaySlot::None},
    {0xF000, 0x9000, Opcode::MovWordPcRelative, Encoding::PcWordLoadDisp8, FlowKind::Fallthrough, DelaySlot::None, 2},
    {0xFF00, 0x8900, Opcode::Bt,        Encoding::Disp8,        FlowKind::ConditionalBranch, DelaySlot::None},
    {0xFF00, 0x8B00, Opcode::Bf,        Encoding::Disp8,        FlowKind::ConditionalBranch, DelaySlot::None},
    {0xFF00, 0x8D00, Opcode::BtS,       Encoding::Disp8,        FlowKind::ConditionalBranch, DelaySlot::Always},
    {0xFF00, 0x8F00, Opcode::BfS,       Encoding::Disp8,        FlowKind::ConditionalBranch, DelaySlot::Always},
    {0xF000, 0xA000, Opcode::Bra,       Encoding::Disp12,       FlowKind::DirectBranch,      DelaySlot::Always},
    {0xF000, 0xB000, Opcode::Bsr,       Encoding::Disp12,       FlowKind::DirectCall,        DelaySlot::Always},
    {0xF000, 0xE000, Opcode::MovImm,    Encoding::RegImm8,      FlowKind::Fallthrough,       DelaySlot::None},
    {0xF000, 0xD000, Opcode::MovLongPcRelative, Encoding::PcLongLoadDisp8, FlowKind::Fallthrough, DelaySlot::None, 4},
    {0xFF00, 0xC800, Opcode::TstImm,    Encoding::UnsignedImmR0, FlowKind::Fallthrough,      DelaySlot::None},
    {0xFF00, 0xC900, Opcode::AndImm,    Encoding::UnsignedImmR0, FlowKind::Fallthrough,      DelaySlot::None},
    {0xFF00, 0xCA00, Opcode::XorImm,    Encoding::UnsignedImmR0, FlowKind::Fallthrough,      DelaySlot::None},
    {0xFF00, 0xCB00, Opcode::OrImm,     Encoding::UnsignedImmR0, FlowKind::Fallthrough,      DelaySlot::None},
    {0xFF00, 0xC300, Opcode::Trapa,     Encoding::UnsignedImm,  FlowKind::Trap,              DelaySlot::None},
    {0xFF00, 0xC700, Opcode::Mova,      Encoding::PcWordDisp8,  FlowKind::Fallthrough,       DelaySlot::None},
};

[[nodiscard]] constexpr std::uint8_t fieldN(std::uint16_t word) noexcept {
    return static_cast<std::uint8_t>((word >> 8U) & 0x0FU);
}

[[nodiscard]] constexpr std::uint8_t fieldM(std::uint16_t word) noexcept {
    return static_cast<std::uint8_t>((word >> 4U) & 0x0FU);
}

void addOperand(DecodedInstruction& instruction, Operand operand) {
    if (instruction.operandCount >= instruction.operands.size()) {
        throw std::logic_error("SH-2 instruction has too many operands");
    }
    instruction.operands[instruction.operandCount++] = std::move(operand);
}

} // namespace

const Operand& DecodedInstruction::operand(std::size_t index) const {
    if (index >= operandCount) {
        throw std::out_of_range("SH-2 operand index out of range");
    }
    return operands[index];
}

DecodeResult Decoder::decode(Address pc, std::uint16_t word) const {
    DecodedInstruction instruction{};
    instruction.address = pc;
    instruction.raw = word;

    for (const auto& pattern : patterns) {
        if ((word & pattern.mask) != pattern.value) {
            continue;
        }

        instruction.valid = true;
        instruction.opcode = pattern.opcode;
        instruction.flow = pattern.flow;
        instruction.delaySlot = pattern.delaySlot;

        const auto n = generalRegister(fieldN(word));
        const auto m = generalRegister(fieldM(word));
        const auto immediate = signExtend<8>(word & 0x00FFU);

        switch (pattern.encoding) {
        case Encoding::Fixed:
            break;
        case Encoding::RegReg:
            addOperand(instruction, RegisterOperand{m});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::RegOnly:
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::RegImm8:
            addOperand(instruction, ImmediateOperand{immediate, 8, true});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::MemoryLoad:
            addOperand(instruction, MemoryOperand{
                m, std::nullopt, 0, pattern.memoryWidth,
                (pattern.opcode == Opcode::MovBytePostIncrement ||
                 pattern.opcode == Opcode::MovWordPostIncrement ||
                 pattern.opcode == Opcode::MovLongPostIncrement)
                    ? MemoryOperand::Mode::PostIncrement
                    : MemoryOperand::Mode::Indirect});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::MemoryStore:
            addOperand(instruction, RegisterOperand{m});
            addOperand(instruction, MemoryOperand{
                n, std::nullopt, 0, pattern.memoryWidth,
                (pattern.opcode == Opcode::MovBytePreDecrement ||
                 pattern.opcode == Opcode::MovWordPreDecrement ||
                 pattern.opcode == Opcode::MovLongPreDecrement)
                    ? MemoryOperand::Mode::PreDecrement
                    : MemoryOperand::Mode::Indirect});
            break;
        case Encoding::Disp8: {
            const auto target = pcRelativeTarget(pc, signExtend<8>(word & 0x00FFU));
            instruction.directTarget = target;
            addOperand(instruction, AddressOperand{target});
            break;
        }
        case Encoding::Disp12: {
            const auto target = pcRelativeTarget(pc, signExtend<12>(word & 0x0FFFU));
            instruction.directTarget = target;
            addOperand(instruction, AddressOperand{target});
            break;
        }
        case Encoding::PcWordDisp8: {
            const auto target = pcRelativeWordTarget(pc, static_cast<std::int32_t>(word & 0x00FFU));
            addOperand(instruction, AddressOperand{target});
            addOperand(instruction, RegisterOperand{Register::R0});
            break;
        }
        case Encoding::PcWordLoadDisp8: {
            const auto target = pcRelativeTarget(pc, static_cast<std::int32_t>(word & 0x00FFU));
            addOperand(instruction, AddressOperand{target});
            addOperand(instruction, RegisterOperand{n});
            break;
        }
        case Encoding::PcLongLoadDisp8: {
            const auto target = pcRelativeWordTarget(pc, static_cast<std::int32_t>(word & 0x00FFU));
            addOperand(instruction, AddressOperand{target});
            addOperand(instruction, RegisterOperand{n});
            break;
        }
        case Encoding::IndirectReg:
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::CompareImmR0:
            addOperand(instruction, ImmediateOperand{immediate, 8, true});
            addOperand(instruction, RegisterOperand{Register::R0});
            break;
        case Encoding::UnsignedImmR0:
            addOperand(instruction, ImmediateOperand{
                static_cast<std::int32_t>(word & 0x00FFU), 8, false});
            addOperand(instruction, RegisterOperand{Register::R0});
            break;
        case Encoding::IndexedMemoryLoad:
            addOperand(instruction, MemoryOperand{
                m, Register::R0, 0, pattern.memoryWidth, MemoryOperand::Mode::Indirect});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::IndexedMemoryStore:
            addOperand(instruction, RegisterOperand{m});
            addOperand(instruction, MemoryOperand{
                n, Register::R0, 0, pattern.memoryWidth, MemoryOperand::Mode::Indirect});
            break;
        case Encoding::DisplacementMemoryLoad:
            addOperand(instruction, MemoryOperand{
                m, std::nullopt,
                static_cast<std::int32_t>((word & 0x000FU) * pattern.memoryWidth),
                pattern.memoryWidth, MemoryOperand::Mode::Indirect});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::DisplacementMemoryStore:
            addOperand(instruction, RegisterOperand{m});
            addOperand(instruction, MemoryOperand{
                n, std::nullopt,
                static_cast<std::int32_t>((word & 0x000FU) * pattern.memoryWidth),
                pattern.memoryWidth, MemoryOperand::Mode::Indirect});
            break;
        case Encoding::DisplacementR0Load:
            addOperand(instruction, MemoryOperand{
                m, std::nullopt,
                static_cast<std::int32_t>((word & 0x000FU) * pattern.memoryWidth),
                pattern.memoryWidth, MemoryOperand::Mode::Indirect});
            addOperand(instruction, RegisterOperand{Register::R0});
            break;
        case Encoding::DisplacementR0Store:
            addOperand(instruction, RegisterOperand{Register::R0});
            addOperand(instruction, MemoryOperand{
                m, std::nullopt,
                static_cast<std::int32_t>((word & 0x000FU) * pattern.memoryWidth),
                pattern.memoryWidth, MemoryOperand::Mode::Indirect});
            break;
        case Encoding::UnsignedImm:
            addOperand(instruction, ImmediateOperand{
                static_cast<std::int32_t>(word & 0x00FFU), 8, false});
            break;
        case Encoding::SrStore:
        case Encoding::GbrStore:
        case Encoding::VbrStore:
            addOperand(instruction, RegisterOperand{
                pattern.encoding == Encoding::SrStore ? Register::SR
                : pattern.encoding == Encoding::GbrStore ? Register::GBR
                                                        : Register::VBR});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::MachStore:
        case Encoding::MaclStore:
        case Encoding::PrStore:
            addOperand(instruction, RegisterOperand{
                pattern.encoding == Encoding::MachStore ? Register::MACH
                : pattern.encoding == Encoding::MaclStore ? Register::MACL
                                                          : Register::PR});
            addOperand(instruction, RegisterOperand{n});
            break;
        case Encoding::SrLoad:
        case Encoding::GbrLoad:
        case Encoding::VbrLoad:
            addOperand(instruction, RegisterOperand{n});
            addOperand(instruction, RegisterOperand{
                pattern.encoding == Encoding::SrLoad ? Register::SR
                : pattern.encoding == Encoding::GbrLoad ? Register::GBR
                                                       : Register::VBR});
            break;
        case Encoding::MachLoad:
        case Encoding::MaclLoad:
        case Encoding::PrLoad:
            addOperand(instruction, RegisterOperand{n});
            addOperand(instruction, RegisterOperand{
                pattern.encoding == Encoding::MachLoad ? Register::MACH
                : pattern.encoding == Encoding::MaclLoad ? Register::MACL
                                                        : Register::PR});
            break;
        case Encoding::StsMachPreDecrement:
        case Encoding::StsMaclPreDecrement:
        case Encoding::StsPrPreDecrement:
            addOperand(instruction, RegisterOperand{
                pattern.encoding == Encoding::StsMachPreDecrement ? Register::MACH
                : pattern.encoding == Encoding::StsMaclPreDecrement ? Register::MACL
                                                                      : Register::PR});
            addOperand(instruction, MemoryOperand{
                n, std::nullopt, 0, pattern.memoryWidth, MemoryOperand::Mode::PreDecrement});
            break;
        case Encoding::LdsMachPostIncrement:
        case Encoding::LdsMaclPostIncrement:
        case Encoding::LdsPrPostIncrement:
            addOperand(instruction, MemoryOperand{
                n, std::nullopt, 0, pattern.memoryWidth, MemoryOperand::Mode::PostIncrement});
            addOperand(instruction, RegisterOperand{
                pattern.encoding == Encoding::LdsMachPostIncrement ? Register::MACH
                : pattern.encoding == Encoding::LdsMaclPostIncrement ? Register::MACL
                                                                      : Register::PR});
            break;
        }

        return DecodeResult{std::move(instruction)};
    }

    return DecodeResult{std::move(instruction)};
}

} // namespace saturn::sh2
