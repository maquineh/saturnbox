#include "saturn/sh2/format.hpp"

#include <iomanip>
#include <iterator>
#include <sstream>
#include <type_traits>
#include <utility>

namespace saturn::sh2 {
namespace {

std::string formatOperand(const Operand& operand) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RegisterOperand>) {
                return std::string(registerName(value.reg));
            } else if constexpr (std::is_same_v<T, ImmediateOperand>) {
                return "#" + std::to_string(value.value);
            } else if constexpr (std::is_same_v<T, DisplacementOperand>) {
                return std::to_string(value.value);
            } else if constexpr (std::is_same_v<T, AddressOperand>) {
                return formatAddress(value.address);
            } else {
                if (value.mode == MemoryOperand::Mode::Indirect && value.index) {
                    return "@(" + std::string(registerName(*value.index)) + "," +
                           std::string(registerName(value.base)) + ")";
                }
                if (value.mode == MemoryOperand::Mode::Indirect && value.displacement != 0) {
                    return "@(" + std::to_string(value.displacement) + "," +
                           std::string(registerName(value.base)) + ")";
                }
                std::string text = value.mode == MemoryOperand::Mode::PreDecrement
                    ? "@-" + std::string(registerName(value.base))
                    : "@" + std::string(registerName(value.base));
                if (value.mode == MemoryOperand::Mode::PostIncrement) {
                    text += "+";
                }
                return text;
            }
        },
        operand);
}

} // namespace

std::string_view opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Invalid: return "INVALID";
    case Opcode::MovReg: return "MOV";
    case Opcode::MovImm: return "MOV";
    case Opcode::MovByteLoad: return "MOV.B";
    case Opcode::MovWordLoad: return "MOV.W";
    case Opcode::MovLongLoad: return "MOV.L";
    case Opcode::MovByteStore: return "MOV.B";
    case Opcode::MovWordStore: return "MOV.W";
    case Opcode::MovLongStore: return "MOV.L";
    case Opcode::MovBytePostIncrement: return "MOV.B";
    case Opcode::MovWordPostIncrement: return "MOV.W";
    case Opcode::MovLongPostIncrement: return "MOV.L";
    case Opcode::MovBytePreDecrement: return "MOV.B";
    case Opcode::MovWordPreDecrement: return "MOV.W";
    case Opcode::MovLongPreDecrement: return "MOV.L";
    case Opcode::MovByteIndexedLoad: return "MOV.B";
    case Opcode::MovWordIndexedLoad: return "MOV.W";
    case Opcode::MovLongIndexedLoad: return "MOV.L";
    case Opcode::MovByteIndexedStore: return "MOV.B";
    case Opcode::MovWordIndexedStore: return "MOV.W";
    case Opcode::MovLongIndexedStore: return "MOV.L";
    case Opcode::MovByteDisplacementLoad: return "MOV.B";
    case Opcode::MovWordDisplacementLoad: return "MOV.W";
    case Opcode::MovLongDisplacementLoad: return "MOV.L";
    case Opcode::MovByteDisplacementStore: return "MOV.B";
    case Opcode::MovWordDisplacementStore: return "MOV.W";
    case Opcode::MovLongDisplacementStore: return "MOV.L";
    case Opcode::MovWordPcRelative: return "MOV.W";
    case Opcode::MovLongPcRelative: return "MOV.L";
    case Opcode::MovT: return "MOVT";
    case Opcode::Mova: return "MOVA";
    case Opcode::StsPrPreDecrement: return "STS.L";
    case Opcode::LdsPrPostIncrement: return "LDS.L";
    case Opcode::Add: return "ADD";
    case Opcode::AddImm: return "ADD";
    case Opcode::AddCarry: return "ADDC";
    case Opcode::AddOverflow: return "ADDV";
    case Opcode::Sub: return "SUB";
    case Opcode::SubCarry: return "SUBC";
    case Opcode::SubOverflow: return "SUBV";
    case Opcode::Neg: return "NEG";
    case Opcode::NegCarry: return "NEGC";
    case Opcode::MulLong: return "MUL.L";
    case Opcode::MulSignedWord: return "MULS.W";
    case Opcode::MulUnsignedWord: return "MULU.W";
    case Opcode::Div0Signed: return "DIV0S";
    case Opcode::Div0Unsigned: return "DIV0U";
    case Opcode::Div1: return "DIV1";
    case Opcode::ExtSignedByte: return "EXTS.B";
    case Opcode::ExtSignedWord: return "EXTS.W";
    case Opcode::ExtUnsignedByte: return "EXTU.B";
    case Opcode::ExtUnsignedWord: return "EXTU.W";
    case Opcode::CmpEq: return "CMP/EQ";
    case Opcode::CmpStr: return "CMP/STR";
    case Opcode::CmpHs: return "CMP/HS";
    case Opcode::CmpGe: return "CMP/GE";
    case Opcode::CmpHi: return "CMP/HI";
    case Opcode::CmpGt: return "CMP/GT";
    case Opcode::CmpEqImm: return "CMP/EQ";
    case Opcode::CmpPz: return "CMP/PZ";
    case Opcode::CmpPl: return "CMP/PL";
    case Opcode::And: return "AND";
    case Opcode::AndImm: return "AND";
    case Opcode::Or: return "OR";
    case Opcode::OrImm: return "OR";
    case Opcode::Xor: return "XOR";
    case Opcode::XorImm: return "XOR";
    case Opcode::Not: return "NOT";
    case Opcode::Tst: return "TST";
    case Opcode::TstImm: return "TST";
    case Opcode::Dt: return "DT";
    case Opcode::Shll: return "SHLL";
    case Opcode::Shll2: return "SHLL2";
    case Opcode::Shll8: return "SHLL8";
    case Opcode::Shll16: return "SHLL16";
    case Opcode::Shlr: return "SHLR";
    case Opcode::Shlr2: return "SHLR2";
    case Opcode::Shlr8: return "SHLR8";
    case Opcode::Shlr16: return "SHLR16";
    case Opcode::Shal: return "SHAL";
    case Opcode::Shar: return "SHAR";
    case Opcode::Rotl: return "ROTL";
    case Opcode::Rotr: return "ROTR";
    case Opcode::Rotcl: return "ROTCL";
    case Opcode::Rotcr: return "ROTCR";
    case Opcode::Bra: return "BRA";
    case Opcode::Braf: return "BRAF";
    case Opcode::Bt: return "BT";
    case Opcode::Bf: return "BF";
    case Opcode::BtS: return "BT/S";
    case Opcode::BfS: return "BF/S";
    case Opcode::Bsr: return "BSR";
    case Opcode::Bsrf: return "BSRF";
    case Opcode::Jmp: return "JMP";
    case Opcode::Jsr: return "JSR";
    case Opcode::Rts: return "RTS";
    case Opcode::Rte: return "RTE";
    case Opcode::Nop: return "NOP";
    case Opcode::Sleep: return "SLEEP";
    case Opcode::Trapa: return "TRAPA";
    case Opcode::Stc: return "STC";
    case Opcode::Sts: return "STS";
    case Opcode::Ldc: return "LDC";
    case Opcode::Lds: return "LDS";
    }
    return "INVALID";
}

std::string_view registerName(Register reg) noexcept {
    constexpr std::string_view names[] = {
        "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
        "PC", "PR", "GBR", "VBR", "MACH", "MACL", "SR",
    };
    const auto index = static_cast<std::size_t>(reg);
    return index < std::size(names) ? names[index] : "?";
}

std::string_view flowName(FlowKind flow) noexcept {
    switch (flow) {
    case FlowKind::Fallthrough: return "fallthrough";
    case FlowKind::DirectBranch: return "direct-branch";
    case FlowKind::ConditionalBranch: return "conditional-branch";
    case FlowKind::IndirectBranch: return "indirect-branch";
    case FlowKind::DirectCall: return "direct-call";
    case FlowKind::IndirectCall: return "indirect-call";
    case FlowKind::Return: return "return";
    case FlowKind::ExceptionReturn: return "exception-return";
    case FlowKind::Trap: return "trap";
    }
    return "unknown";
}

std::string_view delaySlotName(DelaySlot delaySlot) noexcept {
    switch (delaySlot) {
    case DelaySlot::None: return "none";
    case DelaySlot::Always: return "always";
    case DelaySlot::Taken: return "taken";
    }
    return "unknown";
}

std::string formatAddress(Address address) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << address;
    return output.str();
}

std::string formatInstruction(const DecodedInstruction& instruction) {
    std::ostringstream output;
    output << opcodeName(instruction.opcode);
    if (instruction.operandCount != 0) {
        output << "    ";
        for (std::size_t index = 0; index < instruction.operandCount; ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << formatOperand(instruction.operand(index));
        }
    }
    return output.str();
}

} // namespace saturn::sh2
