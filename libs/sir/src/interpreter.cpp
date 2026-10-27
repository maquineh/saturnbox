#include "saturn/sir/interpreter.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace saturn::sir {
namespace {

constexpr unsigned kMemoryGenerationPageShift = 12U;
constexpr std::uint64_t kMaximumCodeSnapshotBytes = 4096U;

struct RuntimeValue {
    Type type{Type::Void};
    std::uint32_t bits{};
};

[[nodiscard]] std::uint32_t normalize(Type type, std::uint32_t bits) noexcept {
    switch (type) {
    case Type::I1: return bits & 1U;
    case Type::I8: return bits & 0xFFU;
    case Type::I16: return bits & 0xFFFFU;
    case Type::I32:
    case Type::Address: return bits;
    case Type::I64:
    case Type::Void: return bits;
    }
    return bits;
}

[[nodiscard]] std::int32_t signed32(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}

struct DivideStep {
    std::uint32_t value{};
    bool q{};
    bool t{};
};

[[nodiscard]] DivideStep divideStep(std::uint32_t dividend, std::uint32_t divisor,
                                    bool oldQ, bool m, bool t) noexcept {
    bool q = (dividend & 0x80000000U) != 0;
    auto value = static_cast<std::uint32_t>((dividend << 1U) | (t ? 1U : 0U));
    std::uint32_t previous{};
    bool carryOrBorrow{};

    if (!oldQ) {
        if (!m) {
            previous = value;
            value = static_cast<std::uint32_t>(value - divisor);
            carryOrBorrow = value > previous;
            q = q ? !carryOrBorrow : carryOrBorrow;
        } else {
            previous = value;
            value = static_cast<std::uint32_t>(value + divisor);
            carryOrBorrow = value < previous;
            q = q ? carryOrBorrow : !carryOrBorrow;
        }
    } else if (!m) {
        previous = value;
        value = static_cast<std::uint32_t>(value + divisor);
        carryOrBorrow = value < previous;
        q = q ? !carryOrBorrow : carryOrBorrow;
    } else {
        previous = value;
        value = static_cast<std::uint32_t>(value - divisor);
        carryOrBorrow = value > previous;
        q = q ? carryOrBorrow : !carryOrBorrow;
    }
    return {value, q, q == m};
}

} // namespace

std::uint32_t MachineState::readState(StateSlot slot) const noexcept {
    if (slot < fastState_.size()) return fastState_[slot];
    const auto found = state_.find(slot);
    return found == state_.end() ? 0U : found->second;
}

void MachineState::writeState(StateSlot slot, std::uint32_t value) {
    if (slot < fastState_.size()) {
        if (fastState_[slot] == value) return;
        fastState_[slot] = value;
        stateSnapshotDirty_ = true;
        return;
    }
    if (value == 0) {
        state_.erase(slot);
    } else {
        state_.insert_or_assign(slot, value);
    }
}

std::uint32_t* MachineState::nativeStateData() noexcept {
    nativeStateExposed_ = true;
    return fastState_.data();
}

std::uint8_t MachineState::readRaw8(core::Address address) const noexcept {
    address = backingAddress(address);
    const auto found = memoryPages_.find(address >> 16U);
    return found == memoryPages_.end() ? 0U : found->second[address & 0xFFFFU];
}

void MachineState::writeRaw8(core::Address address, std::uint8_t value) {
    address = backingAddress(address);
    auto page = memoryPages_.find(address >> 16U);
    if (page == memoryPages_.end()) {
        if (value == 0U) return;
        page = memoryPages_.emplace(address >> 16U, std::vector<std::uint8_t>(0x10000U)).first;
    }
    auto& byte = page->second[address & 0xFFFFU];
    if (byte == value) return;
    byte = value;
    noteMemoryWrite(address);
    memorySnapshotDirty_ = true;
}

std::uint8_t MachineState::read8(core::Address address) const {
    if (interceptor_) {
        if (const auto intercepted = interceptor_->read(address, 1)) {
            return static_cast<std::uint8_t>(*intercepted);
        }
    }
    return readRaw8(address);
}

std::uint16_t MachineState::read16BE(core::Address address) const {
    if (interceptor_) {
        if (const auto intercepted = interceptor_->read(address, 2)) {
            return static_cast<std::uint16_t>(*intercepted);
        }
    }
    address = backingAddress(address);
    const auto offset = address & 0xFFFFU;
    if (offset <= 0xFFFEU) {
        const auto page = memoryPages_.find(address >> 16U);
        if (page == memoryPages_.end()) return 0U;
        const auto& bytes = page->second;
        return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1U]);
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(readRaw8(address)) << 8U) |
                                      readRaw8(static_cast<core::Address>(address + 1U)));
}

std::uint32_t MachineState::read32BE(core::Address address) const {
    if (interceptor_) {
        if (const auto intercepted = interceptor_->read(address, 4)) {
            return *intercepted;
        }
    }
    address = backingAddress(address);
    const auto offset = address & 0xFFFFU;
    if (offset <= 0xFFFCU) {
        const auto page = memoryPages_.find(address >> 16U);
        if (page == memoryPages_.end()) return 0U;
        const auto& bytes = page->second;
        return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
               (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
               bytes[offset + 3U];
    }
    return (static_cast<std::uint32_t>(readRaw8(address)) << 24U) |
           (static_cast<std::uint32_t>(readRaw8(static_cast<core::Address>(address + 1U))) << 16U) |
           (static_cast<std::uint32_t>(readRaw8(static_cast<core::Address>(address + 2U))) << 8U) |
           readRaw8(static_cast<core::Address>(address + 3U));
}

void MachineState::write8(core::Address address, std::uint8_t value) {
    if (!interceptor_ || !interceptor_->write(address, 1, value)) {
        writeRaw8(address, value);
    }
}

void MachineState::write16BE(core::Address address, std::uint16_t value) {
    if (!interceptor_ || !interceptor_->write(address, 2, value)) {
        writeRawWord(address, value, 2U);
    }
}

void MachineState::write32BE(core::Address address, std::uint32_t value) {
    if (!interceptor_ || !interceptor_->write(address, 4, value)) {
        writeRawWord(address, value, 4U);
    }
}

void MachineState::writeRawWord(core::Address address, std::uint32_t value,
                                 std::uint8_t width) {
    address = backingAddress(address);
    const auto offset = address & 0xFFFFU;
    if (offset > 0x10000U - width) {
        for (std::uint8_t byte = 0U; byte < width; ++byte) {
            writeRaw8(static_cast<core::Address>(address + byte),
                static_cast<std::uint8_t>(value >> ((width - byte - 1U) * 8U)));
        }
        return;
    }
    auto page = memoryPages_.find(address >> 16U);
    if (page == memoryPages_.end()) {
        if (value == 0U) return;
        page = memoryPages_.emplace(address >> 16U, std::vector<std::uint8_t>(0x10000U)).first;
    }
    auto lastGenerationPage = std::numeric_limits<core::Address>::max();
    for (std::uint8_t byte = 0U; byte < width; ++byte) {
        auto& destination = page->second[offset + byte];
        const auto next = static_cast<std::uint8_t>(value >> ((width - byte - 1U) * 8U));
        if (destination != next) {
            destination = next;
            const auto changedAddress = static_cast<core::Address>(address + byte);
            const auto generationPage = static_cast<core::Address>(
                changedAddress >> kMemoryGenerationPageShift);
            if (generationPage != lastGenerationPage) {
                noteMemoryWrite(changedAddress);
                lastGenerationPage = generationPage;
            }
            memorySnapshotDirty_ = true;
        }
    }
}

void MachineState::noteMemoryWrite(core::Address address) {
    const auto page = static_cast<core::Address>(address >> kMemoryGenerationPageShift);
    ++memoryPageWriteGenerations_[page];
}

std::uint64_t MachineState::memoryWriteGeneration(
    core::Address address) const noexcept {
    address = backingAddress(address);
    const auto page = static_cast<core::Address>(address >> kMemoryGenerationPageShift);
    const auto found = memoryPageWriteGenerations_.find(page);
    return found == memoryPageWriteGenerations_.end() ? 0U : found->second;
}

void MachineState::setMemoryInterceptor(MemoryInterceptor* interceptor) noexcept {
    interceptor_ = interceptor;
}

core::Address MachineState::backingAddress(core::Address address) const noexcept {
    return interceptor_ ? interceptor_->backingAddress(address) : address;
}

MemoryInterceptor* MachineState::memoryInterceptor() const noexcept { return interceptor_; }

void MachineState::setExecutionContext(const ExecutionContext& context) {
    if (interceptor_) {
        interceptor_->setExecutionContext(context);
    }
}

const std::map<StateSlot, std::uint32_t>& MachineState::stateSlots() const {
    // A hook can request a snapshot between direct native writes in a block.
    // Once exposed, rebuild on every diagnostic request, even after one read.
    if (stateSnapshotDirty_ || nativeStateExposed_) {
        for (StateSlot slot = 0; slot < fastState_.size(); ++slot) {
            if (fastState_[slot] == 0U) state_.erase(slot);
            else state_.insert_or_assign(slot, fastState_[slot]);
        }
        stateSnapshotDirty_ = false;
    }
    return state_;
}
const std::map<core::Address, std::uint8_t>& MachineState::memory() const {
    if (memorySnapshotDirty_) {
        std::map<core::Address, std::uint8_t> snapshot;
        std::vector<core::Address> pages;
        pages.reserve(memoryPages_.size());
        for (const auto& [page, bytes] : memoryPages_) {
            static_cast<void>(bytes);
            pages.push_back(page);
        }
        std::sort(pages.begin(), pages.end());
        for (const auto page : pages) {
            const auto& bytes = memoryPages_.at(page);
            for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
                if (bytes[offset] != 0U) snapshot.emplace_hint(snapshot.end(),
                    static_cast<core::Address>((page << 16U) + offset), bytes[offset]);
            }
        }
        memory_.swap(snapshot);
        memorySnapshotDirty_ = false;
    }
    return memory_;
}
bool MachineState::operator==(const MachineState& other) const {
    if (stateSlots() != other.stateSlots()) return false;
    const auto containsSamePages = [](const auto& pages, const auto& otherPages) {
        for (const auto& [page, bytes] : pages) {
            const auto found = otherPages.find(page);
            if (found != otherPages.end()) {
                if (bytes != found->second) return false;
            } else if (std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte != 0U; })) {
                return false;
            }
        }
        return true;
    };
    return containsSamePages(memoryPages_, other.memoryPages_) &&
           containsSamePages(other.memoryPages_, memoryPages_);
}

ExecutionResult Interpreter::execute(const BasicBlock& block, MachineState& state) const {
    std::unordered_map<std::uint32_t, RuntimeValue> values;
    ExecutionResult result;

    const auto operand = [&](const Instruction& instruction, std::size_t index,
                             RuntimeValue& output) {
        if (index >= instruction.operands.size()) {
            return false;
        }
        const auto found = values.find(instruction.operands[index].id);
        if (found == values.end()) {
            return false;
        }
        output = found->second;
        return true;
    };
    const auto write = [&](Value destination, std::uint32_t bits) {
        values.insert_or_assign(destination.id,
                                RuntimeValue{destination.type, normalize(destination.type, bits)});
    };
    const auto hasResults = [](const Instruction& instruction, std::size_t count) {
        return instruction.results.size() == count;
    };

    for (const auto& instruction : block.instructions()) {
        RuntimeValue left;
        RuntimeValue right;
        RuntimeValue extra;
        const auto binary = [&]() {
            return hasResults(instruction, 1) && operand(instruction, 0, left) &&
                   operand(instruction, 1, right);
        };

        switch (instruction.operation) {
        case Operation::ConstI1:
        case Operation::ConstI32:
            if (!hasResults(instruction, 1)) return {InterpreterStatus::MalformedInstruction};
            write(instruction.results[0], static_cast<std::uint32_t>(instruction.immediate));
            break;
        case Operation::CopyI32:
            if (!hasResults(instruction, 1) || !operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            write(instruction.results[0], left.bits);
            break;
        case Operation::AddI32:
        case Operation::SubI32:
        case Operation::MulI32:
        case Operation::AndI32:
        case Operation::OrI32:
        case Operation::XorI32:
        case Operation::XorI1:
        case Operation::ShiftLeftI32:
        case Operation::LogicalShiftRightI32:
        case Operation::ArithmeticShiftRightI32:
        case Operation::CmpEqI32:
        case Operation::CmpSignedGreaterEqualI32:
        case Operation::CmpUnsignedGreaterEqualI32:
        case Operation::CmpSignedGreaterI32:
        case Operation::CmpUnsignedGreaterI32: {
            if (!binary()) return {InterpreterStatus::MissingValue};
            std::uint32_t output{};
            const auto amount = right.bits & 31U;
            switch (instruction.operation) {
            case Operation::AddI32: output = left.bits + right.bits; break;
            case Operation::SubI32: output = left.bits - right.bits; break;
            case Operation::MulI32: output = left.bits * right.bits; break;
            case Operation::AndI32: output = left.bits & right.bits; break;
            case Operation::OrI32: output = left.bits | right.bits; break;
            case Operation::XorI32:
            case Operation::XorI1: output = left.bits ^ right.bits; break;
            case Operation::ShiftLeftI32: output = left.bits << amount; break;
            case Operation::LogicalShiftRightI32: output = left.bits >> amount; break;
            case Operation::ArithmeticShiftRightI32:
                output = left.bits >> amount;
                if (amount != 0 && (left.bits & 0x80000000U) != 0) {
                    output |= 0xFFFFFFFFU << (32U - amount);
                }
                break;
            case Operation::CmpEqI32: output = left.bits == right.bits; break;
            case Operation::CmpSignedGreaterEqualI32: output = signed32(left.bits) >= signed32(right.bits); break;
            case Operation::CmpUnsignedGreaterEqualI32: output = left.bits >= right.bits; break;
            case Operation::CmpSignedGreaterI32: output = signed32(left.bits) > signed32(right.bits); break;
            case Operation::CmpUnsignedGreaterI32: output = left.bits > right.bits; break;
            default: break;
            }
            write(instruction.results[0], output);
            break;
        }
        case Operation::AddWithCarryI32:
        case Operation::SubWithBorrowI32:
            if (!hasResults(instruction, 2) || !operand(instruction, 0, left) ||
                !operand(instruction, 1, right) || !operand(instruction, 2, extra)) {
                return {InterpreterStatus::MissingValue};
            } else if (instruction.operation == Operation::AddWithCarryI32) {
                const auto wide = static_cast<std::uint64_t>(left.bits) + right.bits +
                                  (extra.bits & 1U);
                write(instruction.results[0], static_cast<std::uint32_t>(wide));
                write(instruction.results[1], static_cast<std::uint32_t>(wide >> 32U));
            } else {
                const auto subtrahend = static_cast<std::uint64_t>(right.bits) +
                                       (extra.bits & 1U);
                write(instruction.results[0], left.bits - static_cast<std::uint32_t>(subtrahend));
                write(instruction.results[1], static_cast<std::uint64_t>(left.bits) < subtrahend);
            }
            break;
        case Operation::AddWithOverflowI32:
        case Operation::SubWithOverflowI32: {
            if (!hasResults(instruction, 2) || !operand(instruction, 0, left) ||
                !operand(instruction, 1, right)) return {InterpreterStatus::MissingValue};
            const auto output = instruction.operation == Operation::AddWithOverflowI32
                                    ? left.bits + right.bits : left.bits - right.bits;
            const auto overflow = instruction.operation == Operation::AddWithOverflowI32
                                    ? ((~(left.bits ^ right.bits) & (left.bits ^ output)) >> 31U)
                                    : (((left.bits ^ right.bits) & (left.bits ^ output)) >> 31U);
            write(instruction.results[0], output);
            write(instruction.results[1], overflow);
            break;
        }
        case Operation::NotI32:
        case Operation::TruncateI32ToI8:
        case Operation::TruncateI32ToI16:
        case Operation::SignExtendI8ToI32:
        case Operation::SignExtendI16ToI32:
        case Operation::ZeroExtendI1ToI32:
            if (!hasResults(instruction, 1) || !operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            if (instruction.operation == Operation::NotI32) write(instruction.results[0], ~left.bits);
            else if (instruction.operation == Operation::TruncateI32ToI8) write(instruction.results[0], left.bits & 0xFFU);
            else if (instruction.operation == Operation::TruncateI32ToI16) write(instruction.results[0], left.bits & 0xFFFFU);
            else if (instruction.operation == Operation::SignExtendI8ToI32) {
                const auto value = left.bits & 0xFFU;
                write(instruction.results[0], (value & 0x80U) ? value | 0xFFFFFF00U : value);
            } else if (instruction.operation == Operation::SignExtendI16ToI32) {
                const auto value = left.bits & 0xFFFFU;
                write(instruction.results[0], (value & 0x8000U) ? value | 0xFFFF0000U : value);
            } else write(instruction.results[0], left.bits & 1U);
            break;
        case Operation::ExtractBitI32:
            if (!hasResults(instruction, 1) || !operand(instruction, 0, left) ||
                instruction.immediate < 0 || instruction.immediate > 31) return {InterpreterStatus::MalformedInstruction};
            write(instruction.results[0], left.bits >> static_cast<unsigned>(instruction.immediate));
            break;
        case Operation::DivideStepI32: {
            RuntimeValue q;
            RuntimeValue m;
            RuntimeValue t;
            if (!hasResults(instruction, 3) || !operand(instruction, 0, left) ||
                !operand(instruction, 1, right) || !operand(instruction, 2, q) ||
                !operand(instruction, 3, m) || !operand(instruction, 4, t)) return {InterpreterStatus::MissingValue};
            const auto step = divideStep(left.bits, right.bits, q.bits != 0, m.bits != 0, t.bits != 0);
            write(instruction.results[0], step.value);
            write(instruction.results[1], step.q);
            write(instruction.results[2], step.t);
            break;
        }
        case Operation::Load8:
        case Operation::Load16:
        case Operation::Load32:
            if (!hasResults(instruction, 1) || !operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            if (instruction.operation == Operation::Load8) write(instruction.results[0], state.read8(left.bits));
            else if (instruction.operation == Operation::Load16) write(instruction.results[0], state.read16BE(left.bits));
            else write(instruction.results[0], state.read32BE(left.bits));
            break;
        case Operation::Store8:
        case Operation::Store16:
        case Operation::Store32:
            if (!operand(instruction, 0, left) || !operand(instruction, 1, right)) return {InterpreterStatus::MissingValue};
            if (instruction.operation == Operation::Store8) state.write8(left.bits, static_cast<std::uint8_t>(right.bits));
            else if (instruction.operation == Operation::Store16) state.write16BE(left.bits, static_cast<std::uint16_t>(right.bits));
            else state.write32BE(left.bits, right.bits);
            break;
        case Operation::ReadState:
            if (!hasResults(instruction, 1)) return {InterpreterStatus::MalformedInstruction};
            write(instruction.results[0], state.readState(instruction.stateSlot));
            break;
        case Operation::WriteState:
            if (!operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            state.writeState(instruction.stateSlot, left.bits);
            break;
        case Operation::Branch:
            result.control = ControlKind::Branch;
            result.target = instruction.target;
            return result;
        case Operation::BranchIf:
            if (!operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            result.control = ControlKind::Branch;
            result.target = left.bits != 0 ? instruction.target : instruction.alternative;
            return result;
        case Operation::BranchIndirect:
            if (!operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            result.control = ControlKind::IndirectBranch;
            result.dynamicTarget = left.bits;
            return result;
        case Operation::Call:
            result.control = ControlKind::Call;
            result.function = instruction.function;
            result.returnSite = instruction.returnSite;
            return result;
        case Operation::CallIndirect:
            if (!operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            result.control = ControlKind::IndirectCall;
            result.dynamicTarget = left.bits;
            result.returnSite = instruction.returnSite;
            return result;
        case Operation::Return:
            if (!operand(instruction, 0, left)) return {InterpreterStatus::MissingValue};
            result.control = ControlKind::Return;
            result.dynamicTarget = left.bits;
            return result;
        case Operation::ReturnFromException:
            result.control = ControlKind::ExceptionReturn;
            return result;
        case Operation::Trap:
            result.control = ControlKind::Trap;
            result.trapVector = static_cast<std::uint8_t>(instruction.immediate);
            return result;
        case Operation::Sleep:
            result.slept = true;
            result.status = InterpreterStatus::Success;
            return result;
        }
    }
    result.status = InterpreterStatus::MissingTerminator;
    return result;
}

ProgramExecutionResult ProgramExecutor::run(BasicBlock* entry, MachineState& state,
                                             const ProgramExecutionOptions& options) const {
    ProgramExecutionResult result;
    // Indirect transfers in the Saturn image repeatedly target a small set of
    // dispatcher blocks. Keep resolution local to this run and invalidate an
    // entry only when a write changes one of its possible code pages.
    struct CachedResolution {
        const BasicBlock* block{};
        std::uint64_t firstPageGeneration{};
        std::uint64_t lastPageGeneration{};
    };
    std::unordered_map<core::Address, CachedResolution> resolutionCache;
    resolutionCache.reserve(256U);
    const auto resolve = [&](const BasicBlock* known, core::Address address) {
        if ((!options.resolveBlock && !options.resolveBlockAtStep) ||
            (!options.refreshKnownBlocksOnWrite && known &&
             !known->instructions().empty())) {
            return known;
        }
        const auto codeEndWide = static_cast<std::uint64_t>(address) +
                                 kMaximumCodeSnapshotBytes - 1U;
        const auto lastCodeAddress = static_cast<core::Address>(std::min(
            codeEndWide, static_cast<std::uint64_t>(
                std::numeric_limits<core::Address>::max())));
        const auto firstPageGeneration = state.memoryWriteGeneration(address);
        const auto lastPageGeneration = state.memoryWriteGeneration(lastCodeAddress);
        const auto cached = resolutionCache.find(address);
        if (cached != resolutionCache.end() &&
            cached->second.firstPageGeneration == firstPageGeneration &&
            cached->second.lastPageGeneration == lastPageGeneration) {
            return cached->second.block;
        }
        const auto* resolved = options.resolveBlockAtStep
            ? options.resolveBlockAtStep(address, result.steps)
            : options.resolveBlock(address);
        resolutionCache.insert_or_assign(address, CachedResolution{
            resolved,
            state.memoryWriteGeneration(address),
            state.memoryWriteGeneration(lastCodeAddress)});
        return resolved;
    };

    const auto* current = entry;
    if (current && options.refreshKnownBlocksOnWrite) {
        current = resolve(current, current->address());
    }
    const BasicBlock* previous = nullptr;
    if (!current || current->instructions().empty()) {
        result.status = ProgramStatus::MissingEntry;
        result.address = current ? current->address() : (entry ? entry->address() : 0U);
        return result;
    }

    Interpreter interpreter;
    struct InterruptFrame {
        const BasicBlock* resume{};
        const BasicBlock* previous{};
        InterruptReturnMode returnMode{InterruptReturnMode::Exception};
        std::array<std::uint32_t, 23> savedCpuState{};
    };
    std::vector<InterruptFrame> interruptFrames;
    constexpr std::uint32_t kSrInterruptMask = 0x000000F0U;
    constexpr std::uint32_t kSrBlock = 0x10000000U;
    constexpr core::Address kCallbackReturnSentinel = 0xFFFFFFFCU;
    const bool publishContext = options.interrupt || options.beforeBlock ||
                                 options.observeBlock || state.memoryInterceptor();
    // Both engines use these same boundaries and control-flow transitions.
    // A native continuation may prepare a successor which needs interpreter
    // fallback; remember that boundary so its hooks are not repeated.
    const auto enterBlock = [&]() -> bool {
        for (;;) {
            result.address = current->address();
            state.writeState(kProgramCounterStateSlot, current->address());
            if (publishContext) {
                ExecutionContext context{current->address(),
                                         previous ? previous->address() : 0U,
                                         result.steps, {}};
                if (state.memoryInterceptor() &&
                    state.memoryInterceptor()->requiresStateSnapshot(current->address())) {
                    context.state = state.stateSlots();
                }
                state.setExecutionContext(context);
            }
            if (options.interrupt) {
                const auto target = options.interrupt(*current, state, result.steps);
                if (target && target->target != 0U) {
                    const auto stack = state.readState(kStackPointerStateSlot);
                    const auto savedSr = state.readState(kStatusRegisterStateSlot);
                    const auto savedPc = current->address();
                    std::array<std::uint32_t, 23> savedCpuState{};
                    if (target->returnMode == InterruptReturnMode::Callback) {
                        for (StateSlot slot = 0; slot < savedCpuState.size(); ++slot) {
                            savedCpuState[slot] = state.readState(slot);
                        }
                    }
                    const auto frameStack = static_cast<core::Address>(stack - 8U);
                    // SH-2 exception frames are exposed as PC followed by SR at
                    // the new SP. Keeping the frame in MachineState also lets an
                    // interrupt handler inspect the same bytes as native code.
                    state.write32BE(frameStack, savedPc);
                    state.write32BE(static_cast<core::Address>(frameStack + 4U), savedSr);
                    state.writeState(kStackPointerStateSlot, frameStack);
                    state.writeState(kStatusRegisterStateSlot, savedSr | kSrBlock |
                                                                kSrInterruptMask);
                    if (target->returnMode == InterruptReturnMode::Callback) {
                        // SYS_SETUINT installs callbacks, not raw exception
                        // handlers. The omitted BIOS wrapper would call them and
                        // execute RTE after their outer RTS; the sentinel models
                        // that wrapper return site without materializing BIOS ROM.
                        state.writeState(kProcedureRegisterStateSlot,
                                         kCallbackReturnSentinel);
                    }
                    interruptFrames.push_back(
                        {current, previous, target->returnMode, savedCpuState});
                    previous = current;
                    current = resolve(nullptr, target->target);
                    if (!current || current->instructions().empty()) {
                        result.status = ProgramStatus::UnresolvedTarget;
                        result.unresolvedTarget = target->target;
                        return false;
                    }
                    continue;
                }
            }
            if (options.refreshKnownBlocksOnWrite) {
                const auto address = current->address();
                current = resolve(current, address);
                if (!current || current->instructions().empty()) {
                    result.status = ProgramStatus::UnresolvedTarget;
                    result.unresolvedTarget = address;
                    return false;
                }
            }
            if (options.beforeBlock) {
                options.beforeBlock(*current, state, result.steps);
            }
            if (options.refreshKnownBlocksOnWrite) {
                const auto address = current->address();
                current = resolve(current, address);
                if (!current || current->instructions().empty()) {
                    result.status = ProgramStatus::UnresolvedTarget;
                    result.unresolvedTarget = address;
                    return false;
                }
            }
            if (options.observeBlock) {
                options.observeBlock(*current, result.steps);
            }
            return true;
        }
    };
    const auto completeBlock = [&](const BasicBlock& block,
                                    const ExecutionResult& execution) -> bool {
        current = &block;
        result.address = block.address();
        result.last = execution;
        ++result.steps;
        if (!result.last) {
            result.status = ProgramStatus::InterpreterFailure;
            return false;
        }
        if (result.last.slept) {
            result.status = ProgramStatus::Sleeping;
            return false;
        }

        previous = current;
        switch (result.last.control) {
        case ControlKind::Branch:
            if (!result.last.target) {
                result.status = ProgramStatus::UnresolvedTarget;
                return false;
            }
            current = resolve(result.last.target, result.last.target->address());
            if (!current || current->instructions().empty()) {
                result.status = ProgramStatus::UnresolvedTarget;
                result.unresolvedTarget = result.last.target->address();
                return false;
            }
            break;
        case ControlKind::IndirectBranch:
        case ControlKind::IndirectCall:
            current = resolve(nullptr, result.last.dynamicTarget);
            if (!current || current->instructions().empty()) {
                result.status = ProgramStatus::UnresolvedTarget;
                result.unresolvedTarget = result.last.dynamicTarget;
                return false;
            }
            break;
        case ControlKind::Call:
            if (!result.last.function) {
                result.status = ProgramStatus::UnresolvedTarget;
                return false;
            }
            current = resolve(result.last.function->entry(),
                              result.last.function->address());
            if (!current || current->instructions().empty()) {
                result.status = ProgramStatus::UnresolvedTarget;
                result.unresolvedTarget = result.last.function->address();
                return false;
            }
            break;
        case ControlKind::Return:
            if (!interruptFrames.empty() &&
                interruptFrames.back().returnMode == InterruptReturnMode::Callback &&
                result.last.dynamicTarget == kCallbackReturnSentinel) {
                const auto stack = state.readState(kStackPointerStateSlot);
                const auto savedPc = state.read32BE(stack);
                const auto savedSr = state.read32BE(
                    static_cast<core::Address>(stack + 4U));
                state.writeState(kStackPointerStateSlot,
                                 static_cast<std::uint32_t>(stack + 8U));
                state.writeState(kProgramCounterStateSlot, savedPc);
                state.writeState(kStatusRegisterStateSlot, savedSr);
                const auto frame = interruptFrames.back();
                interruptFrames.pop_back();
                // The BIOS SCU wrapper saves the interrupted register set
                // before invoking a SYS_SETUINT callback. Restore that set
                // after its outer RTS so asynchronous callbacks cannot
                // corrupt the instruction stream they interrupted.
                for (StateSlot slot = 0; slot < frame.savedCpuState.size(); ++slot) {
                    state.writeState(slot, frame.savedCpuState[slot]);
                }
                state.writeState(kProgramCounterStateSlot, savedPc);
                state.writeState(kStatusRegisterStateSlot, savedSr);
                current = resolve(frame.resume, frame.resume->address());
                if (!current || current->instructions().empty()) {
                    result.status = ProgramStatus::UnresolvedTarget;
                    result.unresolvedTarget = frame.resume->address();
                    return false;
                }
                previous = frame.previous;
                break;
            }
            current = resolve(nullptr, result.last.dynamicTarget);
            if (!current || current->instructions().empty()) {
                result.status = ProgramStatus::Returned;
                return false;
            }
            break;
        case ControlKind::ExceptionReturn:
            if (interruptFrames.empty()) {
                result.status = ProgramStatus::ExceptionReturn;
                return false;
            }
            {
                const auto stack = state.readState(kStackPointerStateSlot);
                const auto savedPc = state.read32BE(stack);
                const auto savedSr = state.read32BE(
                    static_cast<core::Address>(stack + 4U));
                state.writeState(kStackPointerStateSlot,
                                 static_cast<std::uint32_t>(stack + 8U));
                state.writeState(kProgramCounterStateSlot, savedPc);
                state.writeState(kStatusRegisterStateSlot, savedSr);
                const auto frame = interruptFrames.back();
                interruptFrames.pop_back();
                current = resolve(frame.resume, frame.resume->address());
                if (!current || current->instructions().empty()) {
                    result.status = ProgramStatus::UnresolvedTarget;
                    result.unresolvedTarget = frame.resume->address();
                    return false;
                }
                previous = frame.previous;
            }
            break;
        case ControlKind::Trap:
            result.status = ProgramStatus::Trap;
            return false;
        case ControlKind::None:
            result.status = ProgramStatus::InterpreterFailure;
            return false;
        }
        return true;
    };

    bool boundaryPrepared = false;
    while (result.steps < options.maxSteps) {
        if (!boundaryPrepared && !enterBlock()) return result;
        boundaryPrepared = false;
        if (options.executeChain) {
            const auto stepsBefore = result.steps;
            bool running = true;
            const BlockChainContinuation continuation = [&](const BasicBlock& block,
                                                              const ExecutionResult& execution)
                -> const BasicBlock* {
                boundaryPrepared = false;
                if (!completeBlock(block, execution)) {
                    running = false;
                    return nullptr;
                }
                if (result.steps >= options.maxSteps) return nullptr;
                if (!enterBlock()) {
                    running = false;
                    return nullptr;
                }
                boundaryPrepared = true;
                return current;
            };
            const auto chain = options.executeChain(*current, state,
                                                     options.maxSteps - result.steps,
                                                     continuation);
            if (chain) {
                if (!chain->lastBlock || chain->blocksExecuted == 0U ||
                    chain->blocksExecuted != result.steps - stepsBefore ||
                    result.steps > options.maxSteps) {
                    result.status = ProgramStatus::InterpreterFailure;
                    result.last.status = InterpreterStatus::MalformedInstruction;
                    return result;
                }
                if (!running) return result;
                continue;
            }
            // Returning nullopt requires an untouched entry: a backend must
            // never replay a block after completing any part of a chain.
            if (result.steps != stepsBefore || !running || boundaryPrepared) {
                result.status = ProgramStatus::InterpreterFailure;
                result.last.status = InterpreterStatus::MalformedInstruction;
                return result;
            }
        }
        ExecutionResult execution;
        if (options.executeBlock) {
            const auto native = options.executeBlock(*current, state);
            execution = native ? *native : interpreter.execute(*current, state);
        } else {
            execution = interpreter.execute(*current, state);
        }
        if (!completeBlock(*current, execution)) return result;
    }
    result.status = ProgramStatus::StepLimit;
    return result;
}

} // namespace saturn::sir
