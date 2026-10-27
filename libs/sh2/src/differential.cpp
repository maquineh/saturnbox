#include "saturn/sh2/differential.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace saturn::sh2 {
namespace {

[[nodiscard]] Register reg(const DecodedInstruction& instruction, std::size_t index) {
    return std::get<RegisterOperand>(instruction.operand(index)).reg;
}

[[nodiscard]] std::int32_t immediate(const DecodedInstruction& instruction,
                                     std::size_t index = 0) {
    return std::get<ImmediateOperand>(instruction.operand(index)).value;
}

[[nodiscard]] std::uint32_t readRegister(const sir::MachineState& state,
                                         Register regValue) noexcept {
    return state.readState(stateSlot(regValue));
}

void writeRegister(sir::MachineState& state, Register regValue, std::uint32_t value) {
    state.writeState(stateSlot(regValue), value);
}

[[nodiscard]] bool readFlag(const sir::MachineState& state, sir::StateSlot slot) noexcept {
    return state.readState(slot) != 0;
}

void writeFlag(sir::MachineState& state, sir::StateSlot slot, bool value) {
    state.writeState(slot, value ? 1U : 0U);
}

[[nodiscard]] std::int32_t signed32(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}

[[nodiscard]] std::uint32_t signExtend8(std::uint32_t value) noexcept {
    value &= 0xFFU;
    return (value & 0x80U) != 0 ? value | 0xFFFFFF00U : value;
}

[[nodiscard]] std::uint32_t signExtend16(std::uint32_t value) noexcept {
    value &= 0xFFFFU;
    return (value & 0x8000U) != 0 ? value | 0xFFFF0000U : value;
}

[[nodiscard]] Address continuationAddress(const DecodedInstruction& instruction) noexcept {
    return static_cast<Address>(instruction.address + instruction.length +
                                (instruction.delaySlot == DelaySlot::None ? 0U : 2U));
}

[[nodiscard]] std::uint32_t memoryAddress(const MemoryOperand& memory,
                                          const sir::MachineState& state,
                                          std::uint32_t base) noexcept {
    auto address = base + static_cast<std::uint32_t>(memory.displacement);
    if (memory.index) {
        address += readRegister(state, *memory.index);
    }
    return address;
}

[[nodiscard]] bool executeLoad(const DecodedInstruction& instruction,
                               sir::MachineState& state) {
    const auto& memory = std::get<MemoryOperand>(instruction.operand(0));
    const auto destination = reg(instruction, 1);
    const auto base = readRegister(state, memory.base);
    const auto address = memoryAddress(memory, state, base);
    std::uint32_t value{};
    switch (memory.width) {
    case 1: value = signExtend8(state.read8(address)); break;
    case 2: value = signExtend16(state.read16BE(address)); break;
    case 4: value = state.read32BE(address); break;
    default: return false;
    }
    writeRegister(state, destination, value);
    if (memory.mode == MemoryOperand::Mode::PostIncrement && memory.base != destination) {
        writeRegister(state, memory.base, base + memory.width);
    }
    return true;
}

[[nodiscard]] bool executeStore(const DecodedInstruction& instruction,
                                sir::MachineState& state) {
    const auto source = readRegister(state, reg(instruction, 0));
    const auto& memory = std::get<MemoryOperand>(instruction.operand(1));
    auto base = readRegister(state, memory.base);
    if (memory.mode == MemoryOperand::Mode::PreDecrement) {
        base -= memory.width;
        writeRegister(state, memory.base, base);
    }
    const auto address = memoryAddress(memory, state, base);
    switch (memory.width) {
    case 1: state.write8(address, static_cast<std::uint8_t>(source)); break;
    case 2: state.write16BE(address, static_cast<std::uint16_t>(source)); break;
    case 4: state.write32BE(address, source); break;
    default: return false;
    }
    return true;
}

void executeDivideStep(const DecodedInstruction& instruction, sir::MachineState& state) {
    const auto source = readRegister(state, reg(instruction, 0));
    const auto destination = reg(instruction, 1);
    auto value = readRegister(state, destination);
    const auto oldQ = readFlag(state, kStateSlotQ);
    const auto m = readFlag(state, kStateSlotM);
    const auto t = readFlag(state, kStateSlotT);
    auto q = (value & 0x80000000U) != 0;
    value = static_cast<std::uint32_t>((value << 1U) | (t ? 1U : 0U));
    auto previous = value;
    bool carryOrBorrow{};
    if (!oldQ && !m) {
        value -= source;
        carryOrBorrow = value > previous;
        q = q ? !carryOrBorrow : carryOrBorrow;
    } else if (!oldQ && m) {
        value += source;
        carryOrBorrow = value < previous;
        q = q ? carryOrBorrow : !carryOrBorrow;
    } else if (oldQ && !m) {
        value += source;
        carryOrBorrow = value < previous;
        q = q ? !carryOrBorrow : carryOrBorrow;
    } else {
        value -= source;
        carryOrBorrow = value > previous;
        q = q ? carryOrBorrow : !carryOrBorrow;
    }
    writeRegister(state, destination, value);
    writeFlag(state, kStateSlotQ, q);
    writeFlag(state, kStateSlotT, q == m);
}

[[nodiscard]] bool executeInstruction(const DecodedInstruction& instruction,
                                      sir::MachineState& state,
                                      bool& slept) {
    if (!instruction.valid || instruction.flow != FlowKind::Fallthrough) {
        return false;
    }
    switch (instruction.opcode) {
    case Opcode::MovReg:
        writeRegister(state, reg(instruction, 1), readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::MovImm:
        writeRegister(state, reg(instruction, 1), static_cast<std::uint32_t>(immediate(instruction)));
        return true;
    case Opcode::MovT:
        writeRegister(state, reg(instruction, 0), readFlag(state, kStateSlotT) ? 1U : 0U);
        return true;
    case Opcode::Mova:
        writeRegister(state, reg(instruction, 1),
                      std::get<AddressOperand>(instruction.operand(0)).address);
        return true;
    case Opcode::MovByteLoad:
    case Opcode::MovWordLoad:
    case Opcode::MovLongLoad:
    case Opcode::MovBytePostIncrement:
    case Opcode::MovWordPostIncrement:
    case Opcode::MovLongPostIncrement:
    case Opcode::MovByteIndexedLoad:
    case Opcode::MovWordIndexedLoad:
    case Opcode::MovLongIndexedLoad:
    case Opcode::MovByteDisplacementLoad:
    case Opcode::MovWordDisplacementLoad:
    case Opcode::MovLongDisplacementLoad:
    case Opcode::LdsPrPostIncrement:
        return executeLoad(instruction, state);
    case Opcode::MovWordPcRelative:
    case Opcode::MovLongPcRelative: {
        const auto address = std::get<AddressOperand>(instruction.operand(0)).address;
        const auto value = instruction.opcode == Opcode::MovWordPcRelative
                               ? signExtend16(state.read16BE(address))
                               : state.read32BE(address);
        writeRegister(state, reg(instruction, 1), value);
        return true;
    }
    case Opcode::MovByteStore:
    case Opcode::MovWordStore:
    case Opcode::MovLongStore:
    case Opcode::MovBytePreDecrement:
    case Opcode::MovWordPreDecrement:
    case Opcode::MovLongPreDecrement:
    case Opcode::MovByteIndexedStore:
    case Opcode::MovWordIndexedStore:
    case Opcode::MovLongIndexedStore:
    case Opcode::MovByteDisplacementStore:
    case Opcode::MovWordDisplacementStore:
    case Opcode::MovLongDisplacementStore:
    case Opcode::StsPrPreDecrement:
        return executeStore(instruction, state);
    case Opcode::Stc:
    case Opcode::Sts:
    case Opcode::Ldc:
    case Opcode::Lds:
        writeRegister(state, reg(instruction, 1), readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::Add:
        writeRegister(state, reg(instruction, 1),
                      readRegister(state, reg(instruction, 1)) +
                      readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::AddImm:
        writeRegister(state, reg(instruction, 1),
                      readRegister(state, reg(instruction, 1)) +
                      static_cast<std::uint32_t>(immediate(instruction)));
        return true;
    case Opcode::Sub:
        writeRegister(state, reg(instruction, 1),
                      readRegister(state, reg(instruction, 1)) -
                      readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::Neg:
        writeRegister(state, reg(instruction, 1),
                      0U - readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::MulLong:
        writeRegister(state, Register::MACL,
                      readRegister(state, reg(instruction, 1)) *
                      readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::AddCarry:
    case Opcode::SubCarry:
    case Opcode::NegCarry: {
        const auto left = instruction.opcode == Opcode::NegCarry
                              ? 0U : readRegister(state, reg(instruction, 1));
        const auto right = readRegister(state, reg(instruction, 0));
        const auto flag = readFlag(state, kStateSlotT) ? 1U : 0U;
        std::uint32_t result{};
        bool outputFlag{};
        if (instruction.opcode == Opcode::AddCarry) {
            const auto wide = static_cast<std::uint64_t>(left) + right + flag;
            result = static_cast<std::uint32_t>(wide);
            outputFlag = (wide >> 32U) != 0;
        } else {
            const auto subtrahend = static_cast<std::uint64_t>(right) + flag;
            result = left - static_cast<std::uint32_t>(subtrahend);
            outputFlag = static_cast<std::uint64_t>(left) < subtrahend;
        }
        writeRegister(state, reg(instruction, 1), result);
        writeFlag(state, kStateSlotT, outputFlag);
        return true;
    }
    case Opcode::AddOverflow:
    case Opcode::SubOverflow: {
        const auto left = readRegister(state, reg(instruction, 1));
        const auto right = readRegister(state, reg(instruction, 0));
        const auto result = instruction.opcode == Opcode::AddOverflow
                                ? left + right : left - right;
        const auto overflow = instruction.opcode == Opcode::AddOverflow
                                  ? ((~(left ^ right) & (left ^ result)) >> 31U)
                                  : (((left ^ right) & (left ^ result)) >> 31U);
        writeRegister(state, reg(instruction, 1), result);
        writeFlag(state, kStateSlotT, overflow != 0);
        return true;
    }
    case Opcode::ExtUnsignedByte:
    case Opcode::ExtUnsignedWord:
    case Opcode::ExtSignedByte:
    case Opcode::ExtSignedWord: {
        const auto source = readRegister(state, reg(instruction, 0));
        std::uint32_t result{};
        if (instruction.opcode == Opcode::ExtUnsignedByte) result = source & 0xFFU;
        else if (instruction.opcode == Opcode::ExtUnsignedWord) result = source & 0xFFFFU;
        else if (instruction.opcode == Opcode::ExtSignedByte) result = signExtend8(source);
        else result = signExtend16(source);
        writeRegister(state, reg(instruction, 1), result);
        return true;
    }
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor: {
        const auto left = readRegister(state, reg(instruction, 1));
        const auto right = readRegister(state, reg(instruction, 0));
        const auto result = instruction.opcode == Opcode::And ? left & right
                          : instruction.opcode == Opcode::Or ? left | right : left ^ right;
        writeRegister(state, reg(instruction, 1), result);
        return true;
    }
    case Opcode::AndImm:
    case Opcode::OrImm:
    case Opcode::XorImm: {
        const auto left = readRegister(state, reg(instruction, 1));
        const auto right = static_cast<std::uint32_t>(immediate(instruction));
        const auto result = instruction.opcode == Opcode::AndImm ? left & right
                          : instruction.opcode == Opcode::OrImm ? left | right : left ^ right;
        writeRegister(state, reg(instruction, 1), result);
        return true;
    }
    case Opcode::Not:
        writeRegister(state, reg(instruction, 1), ~readRegister(state, reg(instruction, 0)));
        return true;
    case Opcode::CmpEq:
    case Opcode::CmpStr:
    case Opcode::CmpHs:
    case Opcode::CmpGe:
    case Opcode::CmpHi:
    case Opcode::CmpGt: {
        const auto left = readRegister(state, reg(instruction, 1));
        const auto right = readRegister(state, reg(instruction, 0));
        const auto result = instruction.opcode == Opcode::CmpEq ? left == right
                          : instruction.opcode == Opcode::CmpStr ?
                                ((left & 0x000000FFU) == (right & 0x000000FFU) ||
                                 ((left >> 8U) & 0xFFU) == ((right >> 8U) & 0xFFU) ||
                                 ((left >> 16U) & 0xFFU) == ((right >> 16U) & 0xFFU) ||
                                 ((left >> 24U) & 0xFFU) == ((right >> 24U) & 0xFFU))
                          : instruction.opcode == Opcode::CmpHs ? left >= right
                          : instruction.opcode == Opcode::CmpGe ? signed32(left) >= signed32(right)
                          : instruction.opcode == Opcode::CmpHi ? left > right
                                                               : signed32(left) > signed32(right);
        writeFlag(state, kStateSlotT, result);
        return true;
    }
    case Opcode::CmpEqImm:
        writeFlag(state, kStateSlotT,
                  readRegister(state, reg(instruction, 1)) ==
                  static_cast<std::uint32_t>(immediate(instruction)));
        return true;
    case Opcode::CmpPz:
    case Opcode::CmpPl: {
        const auto value = signed32(readRegister(state, reg(instruction, 0)));
        writeFlag(state, kStateSlotT,
                  instruction.opcode == Opcode::CmpPz ? value >= 0 : value > 0);
        return true;
    }
    case Opcode::Tst:
        writeFlag(state, kStateSlotT,
                  (readRegister(state, reg(instruction, 1)) &
                   readRegister(state, reg(instruction, 0))) == 0);
        return true;
    case Opcode::TstImm:
        writeFlag(state, kStateSlotT,
                  (readRegister(state, reg(instruction, 1)) &
                   static_cast<std::uint32_t>(immediate(instruction))) == 0);
        return true;
    case Opcode::Dt: {
        const auto destination = reg(instruction, 0);
        const auto result = readRegister(state, destination) - 1U;
        writeRegister(state, destination, result);
        writeFlag(state, kStateSlotT, result == 0);
        return true;
    }
    case Opcode::Shll2:
    case Opcode::Shll8:
    case Opcode::Shll16:
    case Opcode::Shlr2:
    case Opcode::Shlr8:
    case Opcode::Shlr16: {
        const auto destination = reg(instruction, 0);
        const auto value = readRegister(state, destination);
        const auto amount = instruction.opcode == Opcode::Shll2 || instruction.opcode == Opcode::Shlr2 ? 2U
                          : instruction.opcode == Opcode::Shll8 || instruction.opcode == Opcode::Shlr8 ? 8U : 16U;
        const auto left = instruction.opcode == Opcode::Shll2 ||
                          instruction.opcode == Opcode::Shll8 ||
                          instruction.opcode == Opcode::Shll16;
        writeRegister(state, destination, left ? value << amount : value >> amount);
        return true;
    }
    case Opcode::Shll:
    case Opcode::Shal:
    case Opcode::Shlr:
    case Opcode::Shar: {
        const auto destination = reg(instruction, 0);
        const auto value = readRegister(state, destination);
        const auto left = instruction.opcode == Opcode::Shll || instruction.opcode == Opcode::Shal;
        writeFlag(state, kStateSlotT, ((value >> (left ? 31U : 0U)) & 1U) != 0);
        auto result = left ? value << 1U : value >> 1U;
        if (instruction.opcode == Opcode::Shar && (value & 0x80000000U) != 0) {
            result |= 0x80000000U;
        }
        writeRegister(state, destination, result);
        return true;
    }
    case Opcode::Rotl:
    case Opcode::Rotr:
    case Opcode::Rotcl:
    case Opcode::Rotcr: {
        const auto destination = reg(instruction, 0);
        const auto value = readRegister(state, destination);
        const auto left = instruction.opcode == Opcode::Rotl || instruction.opcode == Opcode::Rotcl;
        const auto outgoing = ((value >> (left ? 31U : 0U)) & 1U) != 0;
        const auto throughCarry = instruction.opcode == Opcode::Rotcl || instruction.opcode == Opcode::Rotcr;
        const auto incoming = throughCarry ? readFlag(state, kStateSlotT) : outgoing;
        const auto result = left ? (value << 1U) | (incoming ? 1U : 0U)
                                 : (value >> 1U) | (incoming ? 0x80000000U : 0U);
        writeRegister(state, destination, result);
        writeFlag(state, kStateSlotT, outgoing);
        return true;
    }
    case Opcode::Div0Signed: {
        const auto m = (readRegister(state, reg(instruction, 0)) & 0x80000000U) != 0;
        const auto q = (readRegister(state, reg(instruction, 1)) & 0x80000000U) != 0;
        writeFlag(state, kStateSlotM, m);
        writeFlag(state, kStateSlotQ, q);
        writeFlag(state, kStateSlotT, m != q);
        return true;
    }
    case Opcode::Div0Unsigned:
        writeFlag(state, kStateSlotM, false);
        writeFlag(state, kStateSlotQ, false);
        writeFlag(state, kStateSlotT, false);
        return true;
    case Opcode::Div1:
        executeDivideStep(instruction, state);
        return true;
    case Opcode::Nop:
        return true;
    case Opcode::Sleep:
        slept = true;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::uint32_t dynamicTarget(const DecodedInstruction& transfer,
                                          const sir::MachineState& state) noexcept {
    auto target = readRegister(state, reg(transfer, 0));
    if (transfer.opcode == Opcode::Braf || transfer.opcode == Opcode::Bsrf) {
        target += transfer.address + 4U;
    }
    return target;
}

[[nodiscard]] ControlObservation observe(const sir::ExecutionResult& result) {
    ControlObservation observation;
    observation.kind = result.control;
    observation.returnSite = result.returnSite
                                 ? std::optional<Address>{result.returnSite->address()}
                                 : std::nullopt;
    observation.trapVector = result.trapVector;
    observation.slept = result.slept;
    switch (result.control) {
    case sir::ControlKind::Branch:
        if (result.target) observation.target = result.target->address();
        break;
    case sir::ControlKind::Call:
        if (result.function) observation.target = result.function->address();
        break;
    case sir::ControlKind::IndirectBranch:
    case sir::ControlKind::IndirectCall:
    case sir::ControlKind::Return:
        observation.target = result.dynamicTarget;
        break;
    case sir::ControlKind::None:
    case sir::ControlKind::ExceptionReturn:
    case sir::ControlKind::Trap:
        break;
    }
    return observation;
}

} // namespace

ReferenceResult ReferenceExecutor::execute(const BasicBlock& block,
                                           sir::MachineState& state) const {
    ReferenceResult result;
    for (const auto& instruction : block.instructions) {
        if (!executeInstruction(instruction, state, result.control.slept)) {
            return {ReferenceStatus::UnsupportedInstruction, instruction.address, {}};
        }
    }

    if (!block.terminator) {
        for (const auto& edge : block.successors) {
            if (edge.kind == EdgeKind::Fallthrough && edge.target) {
                result.control.kind = sir::ControlKind::Branch;
                result.control.target = *edge.target;
                return result;
            }
        }
        return {ReferenceStatus::InvalidBlock, block.start, {}};
    }

    const auto& transfer = block.terminator->transfer;
    if (!transfer.valid || (transfer.delaySlot != DelaySlot::None &&
                            (!block.terminator->slot ||
                             !block.terminator->slot->valid ||
                             block.terminator->slot->flow != FlowKind::Fallthrough))) {
        return {ReferenceStatus::InvalidBlock, transfer.address, {}};
    }

    auto selectedTarget = std::uint32_t{};
    auto condition = false;
    if (transfer.flow == FlowKind::ConditionalBranch) {
        condition = readFlag(state, kStateSlotT);
        if (transfer.opcode == Opcode::Bf || transfer.opcode == Opcode::BfS) {
            condition = !condition;
        }
    } else if (transfer.flow == FlowKind::IndirectBranch ||
               transfer.flow == FlowKind::IndirectCall) {
        selectedTarget = dynamicTarget(transfer, state);
    } else if (transfer.flow == FlowKind::Return) {
        selectedTarget = readRegister(state, Register::PR);
    }

    if (transfer.flow == FlowKind::DirectCall || transfer.flow == FlowKind::IndirectCall) {
        writeRegister(state, Register::PR, continuationAddress(transfer));
    }
    if (block.terminator->slot &&
        !executeInstruction(*block.terminator->slot, state, result.control.slept)) {
        return {ReferenceStatus::UnsupportedInstruction,
                block.terminator->slot->address, {}};
    }

    switch (transfer.flow) {
    case FlowKind::DirectBranch:
        result.control.kind = sir::ControlKind::Branch;
        result.control.target = transfer.directTarget;
        break;
    case FlowKind::ConditionalBranch:
        result.control.kind = sir::ControlKind::Branch;
        result.control.target = condition ? transfer.directTarget
                                          : std::optional<Address>{continuationAddress(transfer)};
        break;
    case FlowKind::IndirectBranch:
        if (transfer.directTarget) {
            result.control.kind = sir::ControlKind::Branch;
            result.control.target = transfer.directTarget;
        } else {
            result.control.kind = sir::ControlKind::IndirectBranch;
            result.control.target = selectedTarget;
        }
        break;
    case FlowKind::DirectCall:
        result.control.kind = sir::ControlKind::Call;
        result.control.target = transfer.directTarget;
        result.control.returnSite = continuationAddress(transfer);
        break;
    case FlowKind::IndirectCall:
        result.control.returnSite = continuationAddress(transfer);
        if (transfer.directTarget) {
            result.control.kind = sir::ControlKind::Call;
            result.control.target = transfer.directTarget;
        } else {
            result.control.kind = sir::ControlKind::IndirectCall;
            result.control.target = selectedTarget;
        }
        break;
    case FlowKind::Return:
        result.control.kind = sir::ControlKind::Return;
        result.control.target = selectedTarget;
        break;
    case FlowKind::ExceptionReturn:
        result.control.kind = sir::ControlKind::ExceptionReturn;
        break;
    case FlowKind::Trap:
        result.control.kind = sir::ControlKind::Trap;
        result.control.trapVector = static_cast<std::uint8_t>(immediate(transfer));
        break;
    case FlowKind::Fallthrough:
        return {ReferenceStatus::InvalidBlock, transfer.address, {}};
    }
    return result;
}

DifferentialResult compareBlock(const BasicBlock& source,
                                const sir::BasicBlock& lowered,
                                const sir::MachineState& initial) {
    auto referenceState = initial;
    auto interpretedState = initial;
    DifferentialResult result;
    result.reference = ReferenceExecutor{}.execute(source, referenceState);
    result.interpreted = sir::Interpreter{}.execute(lowered, interpretedState);
    result.sirControl = observe(result.interpreted);
    result.stateMatches = result.reference && result.interpreted &&
                          referenceState == interpretedState;
    result.controlMatches = result.reference && result.interpreted &&
                            result.reference.control == result.sirControl;
    return result;
}

} // namespace saturn::sh2
