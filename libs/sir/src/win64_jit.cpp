#include "saturn/sir/win64_jit.hpp"
#include "saturn/sir/static_module.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
#define SATURN_SIR_HAS_WIN64_JIT 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#define SATURN_SIR_HAS_WIN64_JIT 0
#endif

namespace saturn::sir {
namespace {

#if SATURN_SIR_HAS_WIN64_JIT

struct RuntimeContext;
using NativeBlockFunction = std::uint32_t (*)(
    std::uint32_t*, std::uint32_t*, RuntimeContext*) noexcept;

struct RuntimeContext {
    MachineState* state{};
    std::exception_ptr failure;
    NativeBlockFunction (*complete)(RuntimeContext*, std::uint32_t) noexcept {};
    std::uint32_t* nextValues{};
    std::uint32_t* nextStates{};
    void* chain{};
};

enum class AccessKind : std::uint32_t {
    Read8, Read16, Read32, Write8, Write16, Write32, ReadState, WriteState,
};

// Exceptions must never unwind through the generated frame (it has no unwind
// metadata). Report failure to the caller, which rethrows after native return.
std::uint64_t accessMachine(RuntimeContext* context, AccessKind kind,
                            std::uint32_t address, std::uint32_t value) noexcept {
    try {
        auto& state = *context->state;
        switch (kind) {
        case AccessKind::Read8: return state.read8(address);
        case AccessKind::Read16: return state.read16BE(address);
        case AccessKind::Read32: return state.read32BE(address);
        case AccessKind::Write8: state.write8(address, static_cast<std::uint8_t>(value)); break;
        case AccessKind::Write16: state.write16BE(address, static_cast<std::uint16_t>(value)); break;
        case AccessKind::Write32: state.write32BE(address, value); break;
        case AccessKind::ReadState: return state.readState(static_cast<StateSlot>(address));
        case AccessKind::WriteState: state.writeState(static_cast<StateSlot>(address), value); break;
        }
        return 0;
    } catch (...) {
        context->failure = std::current_exception();
        return std::uint64_t{1} << 32U;
    }
}

class X64Emitter final {
public:
    void emit(std::uint8_t value) { bytes_.push_back(value); }

    void emit32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            emit(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void loadEaxValue(std::uint32_t slot) { loadEax(slot, 0x83U); }
    void loadEcxValue(std::uint32_t slot) { loadEcx(slot); }
    void storeEaxValue(std::uint32_t slot) { storeEax(slot, 0x83U); }
    void loadEaxState(std::uint32_t slot) { loadEax(slot, 0x82U); }
    void storeEaxState(std::uint32_t slot) { storeEax(slot, 0x82U); }

    void emitPrologue(bool callsHelpers) {
        callsHelpers_ = callsHelpers;
        if (callsHelpers_) {
            // 32 bytes of Win64 shadow space, saved pointers and alignment.
            emit(0x48U); emit(0x83U); emit(0xECU); emit(0x48U); // sub rsp, 72
            emit(0x48U); emit(0x89U); emit(0x4CU); emit(0x24U); emit(0x20U);
            emit(0x48U); emit(0x89U); emit(0x54U); emit(0x24U); emit(0x28U);
            emit(0x4CU); emit(0x89U); emit(0x44U); emit(0x24U); emit(0x30U);
        }
        // Preserve the two Win64 arguments in volatile extended registers.
        emit(0x49U); emit(0x89U); emit(0xCBU); // mov r11, rcx (value slots)
        emit(0x49U); emit(0x89U); emit(0xD2U); // mov r10, rdx (state slots)
    }

    void callAccess(AccessKind kind, std::uint32_t address,
                    bool immediateAddress, std::optional<std::uint32_t> value = {}) {
        emit(0x48U); emit(0x8BU); emit(0x4CU); emit(0x24U); emit(0x30U); // rcx = context
        emit(0xBAU); emit32(static_cast<std::uint32_t>(kind)); // edx = kind
        if (immediateAddress) {
            emit(0x41U); emit(0xB8U); emit32(address); // r8d = state slot
        } else {
            emit(0x45U); emit(0x8BU); emit(0x83U); emit32(address * 4U);
        }
        if (value) {
            emit(0x45U); emit(0x8BU); emit(0x8BU); emit32(*value * 4U);
        } else {
            emit(0x45U); emit(0x31U); emit(0xC9U); // xor r9d, r9d
        }
        emit(0x48U); emit(0xB8U); // mov rax, helper
        const auto helper = reinterpret_cast<std::uintptr_t>(&accessMachine);
        emit32(static_cast<std::uint32_t>(helper));
        emit32(static_cast<std::uint32_t>(helper >> 32U));
        emit(0xFFU); emit(0xD0U); // call rax
        emit(0x48U); emit(0x0FU); emit(0xBAU); emit(0xE0U); emit(0x20U); // bt rax, 32
        emit(0x0FU); emit(0x82U); // jc failure epilogue
        failureBranches_.push_back(bytes_.size());
        emit32(0U);
        emit(0x4CU); emit(0x8BU); emit(0x5CU); emit(0x24U); emit(0x20U); // restore r11
        emit(0x4CU); emit(0x8BU); emit(0x54U); emit(0x24U); emit(0x28U); // restore r10
    }

    void finishHelpers() {
        if (!callsHelpers_) return;
        const auto failureOffset = bytes_.size();
        returnZero(false);
        for (const auto branch : failureBranches_) {
            const auto displacement = static_cast<std::uint32_t>(failureOffset - branch - 4U);
            for (unsigned byte = 0; byte < 4U; ++byte) {
                bytes_[branch + byte] = static_cast<std::uint8_t>(displacement >> (byte * 8U));
            }
        }
    }

    void movEaxImmediate(std::uint32_t value) {
        emit(0xB8U);
        emit32(value);
    }

    void binaryMemory(std::uint8_t opcode, std::uint32_t slot) {
        emit(0x41U);
        emit(opcode);
        emit(0x83U);
        emit32(slot * 4U);
    }

    void multiplyMemory(std::uint32_t slot) {
        emit(0x41U); emit(0x0FU); emit(0xAFU); emit(0x83U);
        emit32(slot * 4U);
    }

    void carryIn(std::uint32_t slot) {
        loadEcxValue(slot);
        emit(0x0FU); emit(0xBAU); emit(0xE1U); emit(0x00U); // bt ecx, 0
    }

    void divideStep(const std::vector<std::uint32_t>& inputs,
                    const std::vector<std::uint32_t>& outputs) {
        loadEaxValue(inputs[0]);
        emit(0x41U); emit(0x89U); emit(0xC0U); // r8d = dividend sign
        emit(0x41U); emit(0xC1U); emit(0xE8U); emit(0x1FU);
        loadEcxValue(inputs[4]);
        emit(0x85U); emit(0xC9U);
        emit(0x0FU); emit(0x95U); emit(0xC1U);
        emit(0x0FU); emit(0xB6U); emit(0xC9U); // ecx = (T != 0)
        emit(0xD1U); emit(0xE0U); // shl eax, 1
        emit(0x09U); emit(0xC8U); // or eax, ecx
        emit(0x45U); emit(0x8BU); emit(0x8BU); emit32(inputs[3] * 4U);
        emit(0x45U); emit(0x85U); emit(0xC9U);
        emit(0x41U); emit(0x0FU); emit(0x95U); emit(0xC1U);
        emit(0x45U); emit(0x0FU); emit(0xB6U); emit(0xC9U); // r9d = (M != 0)
        loadEcxValue(inputs[2]);
        emit(0x85U); emit(0xC9U);
        emit(0x0FU); emit(0x95U); emit(0xC1U);
        emit(0x0FU); emit(0xB6U); emit(0xC9U); // ecx = (old Q != 0)
        emit(0x44U); emit(0x31U); emit(0xC9U); // xor ecx, r9d
        emit(0x75U); emit(0x09U); // jne add (skip seven-byte sub and short jump)
        binaryMemory(0x2BU, inputs[1]);
        emit(0xEBU); emit(0x07U); // jmp merge
        binaryMemory(0x03U, inputs[1]);
        storeEaxValue(outputs[0]); // MOV preserves the carry/borrow flag
        setCondition(0x92U); // setc
        emit(0x44U); emit(0x31U); emit(0xC0U); // eax = sign XOR carry/borrow
        emit(0x89U); emit(0xC1U); // ecx = Q XOR M
        emit(0x44U); emit(0x31U); emit(0xC8U); // eax = Q
        storeEaxValue(outputs[1]);
        emit(0x89U); emit(0xC8U);
        emit(0x83U); emit(0xF0U); emit(0x01U); // eax = (Q == M), new T
        storeEaxValue(outputs[2]);
    }

    void shiftEaxByCl(std::uint8_t modRm) {
        emit(0xD3U);
        emit(modRm);
    }

    void compareEaxMemory(std::uint32_t slot) {
        emit(0x41U); emit(0x3BU); emit(0x83U);
        emit32(slot * 4U);
    }

    void setCondition(std::uint8_t condition) {
        emit(0x0FU); emit(condition); emit(0xC0U); // setcc al
        emit(0x0FU); emit(0xB6U); emit(0xC0U);     // movzx eax, al
    }

    void normalize(Type type) {
        std::uint32_t mask{};
        switch (type) {
        case Type::I1: mask = 0x1U; break;
        case Type::I8: mask = 0xFFU; break;
        case Type::I16: mask = 0xFFFFU; break;
        case Type::I32:
        case Type::Address: return;
        case Type::I64:
        case Type::Void: return;
        }
        emit(0x25U); // and eax, imm32
        emit32(mask);
    }

    void returnEax(bool allowChain = true) {
        if (allowChain) {
            // A single-block call has no continuation. Chains commit the
            // completed block through a noexcept runtime bridge, then jump
            // to the successor without growing the native stack.
            emit(0x48U); emit(0x8BU); emit(0x4CU); emit(0x24U); emit(0x30U);
            emit(0x4CU); emit(0x8BU); emit(0x89U);
            emit32(static_cast<std::uint32_t>(offsetof(RuntimeContext, complete)));
            emit(0x4DU); emit(0x85U); emit(0xC9U); // test r9, r9
            emit(0x0FU); emit(0x84U);
            const auto singleBlock = bytes_.size();
            emit32(0U);
            emit(0x89U); emit(0xC2U); // edx = exit value
            emit(0x41U); emit(0xFFU); emit(0xD1U); // call r9
            emit(0x48U); emit(0x85U); emit(0xC0U); // test rax, rax
            emit(0x0FU); emit(0x84U);
            const auto endChain = bytes_.size();
            emit32(0U);
            emit(0x4CU); emit(0x8BU); emit(0x44U); emit(0x24U); emit(0x30U); // r8 = context
            emit(0x49U); emit(0x8BU); emit(0x88U);
            emit32(static_cast<std::uint32_t>(offsetof(RuntimeContext, nextValues)));
            emit(0x49U); emit(0x8BU); emit(0x90U);
            emit32(static_cast<std::uint32_t>(offsetof(RuntimeContext, nextStates)));
            emit(0x48U); emit(0x83U); emit(0xC4U); emit(0x48U);
            emit(0xFFU); emit(0xE0U); // jmp successor
            for (const auto branch : {singleBlock, endChain}) {
                const auto displacement = static_cast<std::uint32_t>(bytes_.size() - branch - 4U);
                for (unsigned byte = 0; byte < 4U; ++byte) {
                    bytes_[branch + byte] = static_cast<std::uint8_t>(displacement >> (byte * 8U));
                }
            }
        }
        if (callsHelpers_) {
            emit(0x48U); emit(0x83U); emit(0xC4U); emit(0x48U); // add rsp, 72
        }
        emit(0xC3U);
    }
    void returnZero(bool allowChain = true) {
        emit(0x31U); emit(0xC0U); // xor eax, eax
        returnEax(allowChain);
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }

private:
    void loadEax(std::uint32_t slot, std::uint8_t modRm) {
        emit(0x41U); emit(0x8BU); emit(modRm);
        emit32(slot * 4U);
    }

    void loadEcx(std::uint32_t slot) {
        emit(0x41U); emit(0x8BU); emit(0x8BU); // mov ecx, [r11 + disp32]
        emit32(slot * 4U);
    }

    void storeEax(std::uint32_t slot, std::uint8_t modRm) {
        emit(0x41U); emit(0x89U); emit(modRm);
        emit32(slot * 4U);
    }

    std::vector<std::uint8_t> bytes_;
    std::vector<std::size_t> failureBranches_;
    bool callsHelpers_{};
};

[[nodiscard]] bool supportedValueType(Type type) noexcept {
    return type == Type::I1 || type == Type::I8 || type == Type::I16 ||
           type == Type::I32 || type == Type::Address;
}

[[nodiscard]] bool isTerminator(Operation operation) noexcept {
    switch (operation) {
    case Operation::Branch:
    case Operation::BranchIf:
    case Operation::BranchIndirect:
    case Operation::Call:
    case Operation::CallIndirect:
    case Operation::Return:
    case Operation::ReturnFromException:
    case Operation::Trap:
    case Operation::Sleep:
        return true;
    default:
        return false;
    }
}

struct CompiledBlock {
    ~CompiledBlock() {
        if (executableMemory) {
            VirtualFree(executableMemory, 0, MEM_RELEASE);
        }
    }

    void* executableMemory{};
    NativeBlockFunction function{};
    StaticBlockSignature signature{};
    std::size_t valueCount{};
    bool usesMemory{};
    bool usesDenseState{true};
    std::vector<StateSlot> stateSlots;
    std::vector<bool> writtenStateSlots;
    Instruction terminator;
    std::vector<std::uint32_t> valueScratch;
    std::vector<std::uint32_t> stateScratch;
};

[[nodiscard]] std::optional<std::uint32_t> valueOffset(
    const Instruction& instruction, std::size_t index,
    const std::unordered_map<std::uint32_t, std::uint32_t>& values) {
    if (index >= instruction.operands.size() ||
        !supportedValueType(instruction.operands[index].type)) {
        return std::nullopt;
    }
    const auto found = values.find(instruction.operands[index].id);
    if (found == values.end()) {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] bool defineResults(
    const Instruction& instruction, std::size_t expected,
    std::unordered_map<std::uint32_t, std::uint32_t>& values,
    std::uint32_t& nextValue, std::vector<std::uint32_t>& resultSlots) {
    if (instruction.results.size() != expected) return false;
    resultSlots.clear();
    for (const auto& result : instruction.results) {
        if (!supportedValueType(result.type) || values.contains(result.id) ||
            nextValue > static_cast<std::uint32_t>(
                            std::numeric_limits<std::int32_t>::max() / 4)) {
            return false;
        }
        resultSlots.push_back(nextValue);
        values.emplace(result.id, nextValue);
        ++nextValue;
    }
    return true;
}

[[nodiscard]] std::optional<std::unique_ptr<CompiledBlock>> compileBlock(
    const BasicBlock& block) {
    if (!block.isLowered() || block.instructions().empty()) return std::nullopt;

    auto compiled = std::make_unique<CompiledBlock>();
    std::unordered_map<StateSlot, std::uint32_t> stateIndices;
    for (const auto& instruction : block.instructions()) {
        switch (instruction.operation) {
        case Operation::Load8:
        case Operation::Load16:
        case Operation::Load32:
        case Operation::Store8:
        case Operation::Store16:
        case Operation::Store32:
            compiled->usesMemory = true;
            break;
        default: break;
        }
        if (instruction.operation == Operation::ReadState ||
            instruction.operation == Operation::WriteState) {
            if (instruction.stateSlot >= MachineState::kDenseStateSlots) {
                compiled->usesDenseState = false;
            }
            if (!stateIndices.contains(instruction.stateSlot)) {
                const auto index = static_cast<std::uint32_t>(compiled->stateSlots.size());
                stateIndices.emplace(instruction.stateSlot, index);
                compiled->stateSlots.push_back(instruction.stateSlot);
                compiled->writtenStateSlots.push_back(false);
            }
            if (instruction.operation == Operation::WriteState) {
                compiled->writtenStateSlots[stateIndices.at(instruction.stateSlot)] = true;
            }
        }
    }

    X64Emitter emitter;
    emitter.emitPrologue(true);
    std::unordered_map<std::uint32_t, std::uint32_t> valueIndices;
    std::uint32_t nextValue{};
    std::vector<std::uint32_t> resultSlots;
    const auto& instructions = block.instructions();
    bool sawTerminator = false;

    for (std::size_t instructionIndex = 0; instructionIndex < instructions.size();
         ++instructionIndex) {
        const auto& instruction = instructions[instructionIndex];
        if (isTerminator(instruction.operation)) {
            if (instructionIndex + 1U != instructions.size()) return std::nullopt;
            sawTerminator = true;
            compiled->terminator = instruction;
            switch (instruction.operation) {
            case Operation::Branch:
            case Operation::Call:
            case Operation::ReturnFromException:
            case Operation::Trap:
            case Operation::Sleep:
                if (!instruction.operands.empty() || !instruction.results.empty()) {
                    return std::nullopt;
                }
                emitter.returnZero();
                break;
            case Operation::BranchIf: {
                if (!instruction.results.empty() || instruction.operands.size() != 1U) {
                    return std::nullopt;
                }
                const auto condition = valueOffset(instruction, 0, valueIndices);
                if (!condition) return std::nullopt;
                emitter.loadEaxValue(*condition);
                emitter.emit(0x85U); emitter.emit(0xC0U); // test eax, eax
                emitter.setCondition(0x95U);              // setne al
                emitter.returnEax();
                break;
            }
            case Operation::BranchIndirect:
            case Operation::CallIndirect:
            case Operation::Return: {
                if (!instruction.results.empty() || instruction.operands.size() != 1U) {
                    return std::nullopt;
                }
                const auto target = valueOffset(instruction, 0, valueIndices);
                if (!target) return std::nullopt;
                emitter.loadEaxValue(*target);
                emitter.returnEax();
                break;
            }
            default:
                return std::nullopt;
            }
            compiled->valueCount = nextValue;
            break;
        }

        const auto define = [&](std::size_t count) {
            return defineResults(instruction, count, valueIndices, nextValue, resultSlots);
        };
        const auto availableValueCount = nextValue;
        const auto operand = [&](std::size_t index) {
            auto offset = valueOffset(instruction, index, valueIndices);
            if (offset && *offset >= availableValueCount) return std::optional<std::uint32_t>{};
            return offset;
        };
        const auto oneOperandOneResult = [&]() -> std::optional<std::uint32_t> {
            if (instruction.operands.size() != 1U || !define(1U)) return std::nullopt;
            return resultSlots[0];
        };

        switch (instruction.operation) {
        case Operation::ConstI1:
        case Operation::ConstI32: {
            if (!instruction.operands.empty() || !define(1U)) return std::nullopt;
            const auto value = static_cast<std::uint32_t>(instruction.immediate);
            emitter.movEaxImmediate(value);
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(resultSlots[0]);
            break;
        }
        case Operation::CopyI32: {
            const auto output = oneOperandOneResult();
            if (!output) return std::nullopt;
            const auto input = operand(0U);
            if (!input) return std::nullopt;
            emitter.loadEaxValue(*input);
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(*output);
            break;
        }
        case Operation::AddI32:
        case Operation::SubI32:
        case Operation::MulI32:
        case Operation::AndI32:
        case Operation::OrI32:
        case Operation::XorI32:
        case Operation::XorI1:
        case Operation::CmpEqI32:
        case Operation::CmpSignedGreaterEqualI32:
        case Operation::CmpUnsignedGreaterEqualI32:
        case Operation::CmpSignedGreaterI32:
        case Operation::CmpUnsignedGreaterI32: {
            if (instruction.operands.size() != 2U || !define(1U)) return std::nullopt;
            const auto left = operand(0U);
            const auto right = operand(1U);
            if (!left || !right) return std::nullopt;
            emitter.loadEaxValue(*left);
            switch (instruction.operation) {
            case Operation::AddI32: emitter.binaryMemory(0x03U, *right); break;
            case Operation::SubI32: emitter.binaryMemory(0x2BU, *right); break;
            case Operation::AndI32: emitter.binaryMemory(0x23U, *right); break;
            case Operation::OrI32: emitter.binaryMemory(0x0BU, *right); break;
            case Operation::XorI32:
            case Operation::XorI1: emitter.binaryMemory(0x33U, *right); break;
            case Operation::MulI32: emitter.multiplyMemory(*right); break;
            case Operation::CmpEqI32:
            case Operation::CmpSignedGreaterEqualI32:
            case Operation::CmpUnsignedGreaterEqualI32:
            case Operation::CmpSignedGreaterI32:
            case Operation::CmpUnsignedGreaterI32:
                emitter.compareEaxMemory(*right);
                if (instruction.operation == Operation::CmpEqI32) emitter.setCondition(0x94U);
                else if (instruction.operation == Operation::CmpSignedGreaterEqualI32) emitter.setCondition(0x9DU);
                else if (instruction.operation == Operation::CmpUnsignedGreaterEqualI32) emitter.setCondition(0x93U);
                else if (instruction.operation == Operation::CmpSignedGreaterI32) emitter.setCondition(0x9FU);
                else emitter.setCondition(0x97U);
                break;
            default: return std::nullopt;
            }
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(resultSlots[0]);
            break;
        }
        case Operation::AddWithCarryI32:
        case Operation::SubWithBorrowI32:
        case Operation::AddWithOverflowI32:
        case Operation::SubWithOverflowI32: {
            const bool withCarry = instruction.operation == Operation::AddWithCarryI32 ||
                                   instruction.operation == Operation::SubWithBorrowI32;
            if (instruction.operands.size() != (withCarry ? 3U : 2U) || !define(2U)) {
                return std::nullopt;
            }
            const auto left = operand(0U);
            const auto right = operand(1U);
            if (!left || !right) return std::nullopt;
            emitter.loadEaxValue(*left);
            if (withCarry) {
                const auto carry = operand(2U);
                if (!carry) return std::nullopt;
                emitter.carryIn(*carry);
                emitter.binaryMemory(instruction.operation == Operation::AddWithCarryI32
                                         ? 0x13U : 0x1BU, *right); // ADC / SBB
            } else {
                emitter.binaryMemory(instruction.operation == Operation::AddWithOverflowI32
                                         ? 0x03U : 0x2BU, *right);
            }
            emitter.storeEaxValue(resultSlots[0]);
            emitter.setCondition(withCarry ? 0x92U : 0x90U); // CF / OF
            emitter.normalize(instruction.results[1].type);
            emitter.storeEaxValue(resultSlots[1]);
            emitter.loadEaxValue(resultSlots[0]);
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(resultSlots[0]);
            break;
        }
        case Operation::DivideStepI32: {
            if (instruction.operands.size() != 5U || !define(3U) ||
                instruction.results[0].type != Type::I32 ||
                instruction.results[1].type != Type::I1 ||
                instruction.results[2].type != Type::I1) return std::nullopt;
            std::vector<std::uint32_t> inputs;
            for (std::size_t index = 0; index < 5U; ++index) {
                const auto input = operand(index);
                if (!input) return std::nullopt;
                inputs.push_back(*input);
            }
            emitter.divideStep(inputs, resultSlots);
            break;
        }
        case Operation::ShiftLeftI32:
        case Operation::LogicalShiftRightI32:
        case Operation::ArithmeticShiftRightI32: {
            if (instruction.operands.size() != 2U || !define(1U)) return std::nullopt;
            const auto value = operand(0U);
            const auto amount = operand(1U);
            if (!value || !amount) return std::nullopt;
            emitter.loadEaxValue(*value);
            emitter.loadEcxValue(*amount);
            if (instruction.operation == Operation::ShiftLeftI32) emitter.shiftEaxByCl(0xE0U);
            else if (instruction.operation == Operation::LogicalShiftRightI32) emitter.shiftEaxByCl(0xE8U);
            else emitter.shiftEaxByCl(0xF8U);
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(resultSlots[0]);
            break;
        }
        case Operation::NotI32:
        case Operation::TruncateI32ToI8:
        case Operation::TruncateI32ToI16:
        case Operation::SignExtendI8ToI32:
        case Operation::SignExtendI16ToI32:
        case Operation::ZeroExtendI1ToI32: {
            const auto output = oneOperandOneResult();
            if (!output) return std::nullopt;
            const auto input = operand(0U);
            if (!input) return std::nullopt;
            emitter.loadEaxValue(*input);
            switch (instruction.operation) {
            case Operation::NotI32:
                emitter.emit(0xF7U); emitter.emit(0xD0U); // not eax
                break;
            case Operation::TruncateI32ToI8:
                emitter.emit(0x25U); emitter.emit32(0xFFU);
                break;
            case Operation::TruncateI32ToI16:
                emitter.emit(0x25U); emitter.emit32(0xFFFFU);
                break;
            case Operation::SignExtendI8ToI32:
                emitter.emit(0x0FU); emitter.emit(0xBEU); emitter.emit(0xC0U);
                break;
            case Operation::SignExtendI16ToI32:
                emitter.emit(0x0FU); emitter.emit(0xBFU); emitter.emit(0xC0U);
                break;
            case Operation::ZeroExtendI1ToI32:
                emitter.emit(0x83U); emitter.emit(0xE0U); emitter.emit(0x01U);
                break;
            default: return std::nullopt;
            }
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(*output);
            break;
        }
        case Operation::ExtractBitI32: {
            const auto output = oneOperandOneResult();
            if (!output || instruction.immediate < 0 || instruction.immediate > 31) {
                return std::nullopt;
            }
            const auto input = operand(0U);
            if (!input) return std::nullopt;
            emitter.loadEaxValue(*input);
            emitter.emit(0xC1U); emitter.emit(0xE8U);
            emitter.emit(static_cast<std::uint8_t>(instruction.immediate));
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(*output);
            break;
        }
        case Operation::ReadState: {
            if (!instruction.operands.empty() || !define(1U)) return std::nullopt;
            if (compiled->usesDenseState) {
                emitter.loadEaxState(instruction.stateSlot);
            } else if (compiled->usesMemory) {
                emitter.callAccess(AccessKind::ReadState, instruction.stateSlot, true);
            } else {
                emitter.loadEaxState(stateIndices.at(instruction.stateSlot));
            }
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(resultSlots[0]);
            break;
        }
        case Operation::WriteState: {
            if (!instruction.results.empty() || instruction.operands.size() != 1U) {
                return std::nullopt;
            }
            const auto value = operand(0U);
            if (!value) return std::nullopt;
            if (compiled->usesDenseState) {
                emitter.loadEaxValue(*value);
                emitter.storeEaxState(instruction.stateSlot);
            } else if (compiled->usesMemory) {
                emitter.callAccess(AccessKind::WriteState, instruction.stateSlot, true, *value);
            } else {
                emitter.loadEaxValue(*value);
                emitter.storeEaxState(stateIndices.at(instruction.stateSlot));
            }
            break;
        }
        case Operation::Load8:
        case Operation::Load16:
        case Operation::Load32: {
            const auto output = oneOperandOneResult();
            if (!output) return std::nullopt;
            const auto address = operand(0U);
            if (!address) return std::nullopt;
            const auto kind = instruction.operation == Operation::Load8 ? AccessKind::Read8 :
                              instruction.operation == Operation::Load16 ? AccessKind::Read16 :
                                                                          AccessKind::Read32;
            emitter.callAccess(kind, *address, false);
            emitter.normalize(instruction.results[0].type);
            emitter.storeEaxValue(*output);
            break;
        }
        case Operation::Store8:
        case Operation::Store16:
        case Operation::Store32: {
            if (!instruction.results.empty() || instruction.operands.size() != 2U) {
                return std::nullopt;
            }
            const auto address = operand(0U);
            const auto value = operand(1U);
            if (!address || !value) return std::nullopt;
            const auto kind = instruction.operation == Operation::Store8 ? AccessKind::Write8 :
                              instruction.operation == Operation::Store16 ? AccessKind::Write16 :
                                                                           AccessKind::Write32;
            emitter.callAccess(kind, *address, false, *value);
            break;
        }
        default:
            return std::nullopt;
        }
    }

    if (!sawTerminator) return std::nullopt;
    emitter.finishHelpers();

    const auto& code = emitter.bytes();
    if (code.empty()) return std::nullopt;
    auto* memory = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!memory) return std::nullopt;
    std::memcpy(memory, code.data(), code.size());
    DWORD previousProtection{};
    if (!VirtualProtect(memory, code.size(), PAGE_EXECUTE_READ, &previousProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, code.size())) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return std::nullopt;
    }
    compiled->executableMemory = memory;
    compiled->function = reinterpret_cast<NativeBlockFunction>(memory);
    return std::move(compiled);
}

[[nodiscard]] ExecutionResult makeExecutionResult(const Instruction& terminator,
                                                   std::uint32_t value) {
    ExecutionResult result;
    switch (terminator.operation) {
    case Operation::Branch:
        result.control = ControlKind::Branch;
        result.target = terminator.target;
        break;
    case Operation::BranchIf:
        result.control = ControlKind::Branch;
        result.target = value != 0U ? terminator.target : terminator.alternative;
        break;
    case Operation::BranchIndirect:
        result.control = ControlKind::IndirectBranch;
        result.dynamicTarget = value;
        break;
    case Operation::Call:
        result.control = ControlKind::Call;
        result.function = terminator.function;
        result.returnSite = terminator.returnSite;
        break;
    case Operation::CallIndirect:
        result.control = ControlKind::IndirectCall;
        result.dynamicTarget = value;
        result.returnSite = terminator.returnSite;
        break;
    case Operation::Return:
        result.control = ControlKind::Return;
        result.dynamicTarget = value;
        break;
    case Operation::ReturnFromException:
        result.control = ControlKind::ExceptionReturn;
        break;
    case Operation::Trap:
        result.control = ControlKind::Trap;
        result.trapVector = static_cast<std::uint8_t>(terminator.immediate);
        break;
    case Operation::Sleep:
        result.slept = true;
        break;
    default:
        result.status = InterpreterStatus::MalformedInstruction;
        break;
    }
    return result;
}

#endif

} // namespace

struct Win64JitBlockExecutor::Impl {
#if SATURN_SIR_HAS_WIN64_JIT
    std::unordered_map<const BasicBlock*, std::unique_ptr<CompiledBlock>> compiled;
    std::unordered_map<const BasicBlock*, StaticBlockSignature> unsupported;
    std::size_t executedBlocks{};
    std::size_t chainedTransfers{};

    CompiledBlock* findOrCompile(const BasicBlock& block) {
        const auto signature = staticBlockSignature(block);
        auto found = compiled.find(&block);
        if (found != compiled.end() && found->second->signature != signature) {
            compiled.erase(found);
            found = compiled.end();
        }
        if (found == compiled.end()) {
            const auto failed = unsupported.find(&block);
            if (failed != unsupported.end() && failed->second == signature) return nullptr;
            auto result = compileBlock(block);
            if (!result) {
                if (block.isLowered()) unsupported.insert_or_assign(&block, signature);
                return nullptr;
            }
            (*result)->signature = signature;
            unsupported.erase(&block);
            found = compiled.emplace(&block, std::move(*result)).first;
        }
        return found->second.get();
    }

    static void prepare(CompiledBlock& block, RuntimeContext& context) {
        block.valueScratch.resize(block.valueCount);
        context.nextValues = block.valueScratch.data();
        if (block.usesDenseState) {
            context.nextStates = context.state->nativeStateData();
            return;
        }
        block.stateScratch.resize(block.stateSlots.size());
        if (!block.usesMemory) {
            for (std::size_t index = 0; index < block.stateSlots.size(); ++index) {
                block.stateScratch[index] = context.state->readState(block.stateSlots[index]);
            }
        }
        context.nextStates = block.stateScratch.data();
    }

    static void commit(CompiledBlock& block, MachineState& state) {
        if (block.usesDenseState) return;
        if (!block.usesMemory) {
            for (std::size_t index = 0; index < block.stateSlots.size(); ++index) {
                if (block.writtenStateSlots[index]) {
                    state.writeState(block.stateSlots[index], block.stateScratch[index]);
                }
            }
        }
    }

    struct Chain {
        Impl* owner{};
        CompiledBlock* current{};
        const BasicBlock* source{};
        std::size_t maxBlocks{};
        BlockChainResult result;
        const BlockChainContinuation* continuation{};
    };

    static NativeBlockFunction completeChain(RuntimeContext* context,
                                              std::uint32_t exitValue) noexcept {
        try {
            auto& chain = *static_cast<Chain*>(context->chain);
            commit(*chain.current, *context->state);
            chain.result.last = makeExecutionResult(chain.current->terminator, exitValue);
            chain.result.lastBlock = chain.source;
            ++chain.result.blocksExecuted;
            ++chain.owner->executedBlocks;
            const BasicBlock* next = nullptr;
            if (chain.continuation && *chain.continuation) {
                next = (*chain.continuation)(*chain.source, chain.result.last);
            } else if (chain.result.last.control == ControlKind::Branch) {
                next = chain.result.last.target;
            } else if (chain.result.last.control == ControlKind::Call &&
                       chain.result.last.function) {
                next = chain.result.last.function->entry();
            }
            if (chain.result.blocksExecuted >= chain.maxBlocks) return nullptr;
            if (!next || next->instructions().empty()) return nullptr;
            auto* compiledNext = chain.owner->findOrCompile(*next);
            if (!compiledNext) return nullptr;
            // ProgramExecutor performs this same PC update at every boundary.
            if (!chain.continuation || !*chain.continuation) {
                context->state->writeState(kProgramCounterStateSlot, next->address());
            }
            prepare(*compiledNext, *context);
            chain.current = compiledNext;
            chain.source = next;
            ++chain.owner->chainedTransfers;
            return compiledNext->function;
        } catch (...) {
            context->failure = std::current_exception();
            return nullptr;
        }
    }
#endif
};

Win64JitBlockExecutor::Win64JitBlockExecutor() : impl_(std::make_unique<Impl>()) {}
Win64JitBlockExecutor::~Win64JitBlockExecutor() = default;

bool Win64JitBlockExecutor::available() noexcept {
#if SATURN_SIR_HAS_WIN64_JIT
    return true;
#else
    return false;
#endif
}

std::optional<ExecutionResult> Win64JitBlockExecutor::execute(const BasicBlock& block,
                                                              MachineState& state) {
#if SATURN_SIR_HAS_WIN64_JIT
    auto* compiled = impl_->findOrCompile(block);
    if (!compiled) return std::nullopt;
    RuntimeContext context{&state, {}};
    Impl::prepare(*compiled, context);
    const auto exitValue = compiled->function(context.nextValues, context.nextStates, &context);
    if (context.failure) std::rethrow_exception(context.failure);
    Impl::commit(*compiled, state);
    ++impl_->executedBlocks;
    return makeExecutionResult(compiled->terminator, exitValue);
#else
    (void)block;
    (void)state;
    return std::nullopt;
#endif
}

std::optional<BlockChainResult> Win64JitBlockExecutor::executeChain(
    const BasicBlock& entry, MachineState& state, std::size_t maxBlocks,
    const BlockChainContinuation& continuation) {
#if SATURN_SIR_HAS_WIN64_JIT
    if (maxBlocks == 0U || (state.memoryInterceptor() && !continuation)) return std::nullopt;
    auto* compiled = impl_->findOrCompile(entry);
    if (!compiled) return std::nullopt;
    Impl::Chain chain{impl_.get(), compiled, &entry, maxBlocks, {}, &continuation};
    RuntimeContext context{&state, {}, &Impl::completeChain, nullptr, nullptr, &chain};
    if (!continuation) state.writeState(kProgramCounterStateSlot, entry.address());
    Impl::prepare(*compiled, context);
    (void)compiled->function(context.nextValues, context.nextStates, &context);
    if (context.failure) std::rethrow_exception(context.failure);
    return chain.result;
#else
    (void)entry;
    (void)state;
    (void)maxBlocks;
    (void)continuation;
    return std::nullopt;
#endif
}

std::size_t Win64JitBlockExecutor::compiledBlockCount() const noexcept {
#if SATURN_SIR_HAS_WIN64_JIT
    return impl_->compiled.size();
#else
    return 0U;
#endif
}

std::size_t Win64JitBlockExecutor::executedBlockCount() const noexcept {
#if SATURN_SIR_HAS_WIN64_JIT
    return impl_->executedBlocks;
#else
    return 0U;
#endif
}

std::size_t Win64JitBlockExecutor::chainedTransferCount() const noexcept {
#if SATURN_SIR_HAS_WIN64_JIT
    return impl_->chainedTransfers;
#else
    return 0U;
#endif
}

} // namespace saturn::sir
