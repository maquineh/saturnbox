#include "saturn/sh2/block_lifter.hpp"

#include <cstddef>
#include <variant>

namespace saturn::sh2 {
namespace {

[[nodiscard]] Register registerOperand(const DecodedInstruction& instruction,
                                       std::size_t index = 0) {
    return std::get<RegisterOperand>(instruction.operand(index)).reg;
}

[[nodiscard]] Address continuationAddress(const DecodedInstruction& instruction) noexcept {
    return static_cast<Address>(instruction.address + instruction.length +
                                (instruction.delaySlot == DelaySlot::None ? 0U : 2U));
}

[[nodiscard]] BlockLiftResult failure(BlockLiftStatus status, Address address) noexcept {
    return {status, address};
}

[[nodiscard]] bool canLift(const DecodedInstruction& instruction) noexcept {
    return instruction.valid && instruction.flow == FlowKind::Fallthrough &&
           lifterFor(instruction.opcode) != nullptr;
}

[[nodiscard]] const CFGEdge* fallthroughEdge(const BasicBlock& block) noexcept {
    for (const auto& edge : block.successors) {
        if (edge.kind == EdgeKind::Fallthrough) {
            return &edge;
        }
    }
    return nullptr;
}

struct ResolvedTargets {
    sir::BasicBlock* taken{};
    sir::BasicBlock* notTaken{};
    sir::BasicBlock* returnSite{};
    sir::Function* function{};
};

[[nodiscard]] BlockLiftResult resolveTargets(const BasicBlock& block,
                                             BlockLiftContext& context,
                                             ResolvedTargets& targets) {
    if (!block.terminator) {
        const auto* edge = fallthroughEdge(block);
        if (!edge || !edge->target) {
            return failure(BlockLiftStatus::MissingSuccessor, block.start);
        }
        targets.taken = context.blockFor(*edge->target);
        return targets.taken ? BlockLiftResult{}
                             : failure(BlockLiftStatus::MissingTarget, *edge->target);
    }

    const auto& transfer = block.terminator->transfer;
    const auto resolveBlock = [&](Address address, sir::BasicBlock*& destination) {
        destination = context.blockFor(address);
        return destination != nullptr;
    };

    switch (transfer.flow) {
    case FlowKind::DirectBranch:
        if (!transfer.directTarget) {
            return failure(BlockLiftStatus::MissingTarget, transfer.address);
        }
        if (!resolveBlock(*transfer.directTarget, targets.taken)) {
            return failure(BlockLiftStatus::MissingTarget, *transfer.directTarget);
        }
        break;
    case FlowKind::ConditionalBranch:
        if (!transfer.directTarget) {
            return failure(BlockLiftStatus::MissingTarget, transfer.address);
        }
        if (!resolveBlock(*transfer.directTarget, targets.taken)) {
            return failure(BlockLiftStatus::MissingTarget, *transfer.directTarget);
        }
        if (!resolveBlock(continuationAddress(transfer), targets.notTaken)) {
            return failure(BlockLiftStatus::MissingTarget, continuationAddress(transfer));
        }
        break;
    case FlowKind::DirectCall:
        if (!transfer.directTarget) {
            return failure(BlockLiftStatus::MissingTarget, transfer.address);
        }
        targets.function = context.functionFor(*transfer.directTarget);
        if (!targets.function) {
            return failure(BlockLiftStatus::MissingTarget, *transfer.directTarget);
        }
        if (!resolveBlock(continuationAddress(transfer), targets.returnSite)) {
            return failure(BlockLiftStatus::MissingTarget, continuationAddress(transfer));
        }
        break;
    case FlowKind::IndirectCall:
        if (transfer.directTarget) {
            targets.function = context.functionFor(*transfer.directTarget);
            if (!targets.function) {
                return failure(BlockLiftStatus::MissingTarget, *transfer.directTarget);
            }
        }
        if (!resolveBlock(continuationAddress(transfer), targets.returnSite)) {
            return failure(BlockLiftStatus::MissingTarget, continuationAddress(transfer));
        }
        break;
    case FlowKind::IndirectBranch:
        if (transfer.directTarget && !resolveBlock(*transfer.directTarget, targets.taken)) {
            return failure(BlockLiftStatus::MissingTarget, *transfer.directTarget);
        }
        break;
    case FlowKind::Return:
    case FlowKind::ExceptionReturn:
    case FlowKind::Trap:
        break;
    case FlowKind::Fallthrough:
        return failure(BlockLiftStatus::InvalidInstruction, transfer.address);
    }
    return {};
}

[[nodiscard]] sir::Value indirectTarget(const DecodedInstruction& transfer,
                                        BlockLiftContext& context) {
    auto target = context.read(registerOperand(transfer));
    if (transfer.opcode == Opcode::Braf || transfer.opcode == Opcode::Bsrf) {
        target = context.builder().addI32(
            target,
            context.builder().constI32(static_cast<std::int32_t>(transfer.address + 4U)));
    }
    return target;
}

} // namespace

BlockLiftResult lowerBlock(const BasicBlock& block, BlockLiftContext& context) {
    for (const auto& instruction : block.instructions) {
        if (!instruction.valid) {
            return failure(BlockLiftStatus::InvalidInstruction, instruction.address);
        }
        if (!canLift(instruction)) {
            return failure(BlockLiftStatus::UnsupportedInstruction, instruction.address);
        }
    }

    if (block.terminator) {
        const auto& transfer = block.terminator->transfer;
        if (!transfer.valid) {
            return failure(BlockLiftStatus::InvalidInstruction, transfer.address);
        }
        if (transfer.delaySlot != DelaySlot::None) {
            if (!block.terminator->slot) {
                return failure(BlockLiftStatus::MissingDelaySlot, transfer.address);
            }
            const auto& slot = *block.terminator->slot;
            if (!slot.valid || slot.flow != FlowKind::Fallthrough) {
                return failure(BlockLiftStatus::InvalidDelaySlot, slot.address);
            }
            if (!canLift(slot)) {
                return failure(BlockLiftStatus::UnsupportedInstruction, slot.address);
            }
        }
    }

    ResolvedTargets targets;
    if (const auto result = resolveTargets(block, context, targets); !result) {
        return result;
    }

    for (const auto& instruction : block.instructions) {
        if (liftInstruction(instruction, context) != LiftResult::Success) {
            return failure(BlockLiftStatus::UnsupportedInstruction, instruction.address);
        }
    }

    if (!block.terminator) {
        context.builder().branch(targets.taken);
        return {};
    }

    const auto& transfer = block.terminator->transfer;
    auto condition = sir::Value{};
    auto dynamicTarget = sir::Value{};

    // These values are architecturally selected before the delayed instruction.
    if (transfer.flow == FlowKind::ConditionalBranch) {
        condition = context.readT();
        if (transfer.opcode == Opcode::Bf || transfer.opcode == Opcode::BfS) {
            condition = context.builder().xorI1(condition, context.builder().constI1(true));
        }
    } else if ((transfer.flow == FlowKind::IndirectBranch ||
                transfer.flow == FlowKind::IndirectCall) &&
               !transfer.directTarget) {
        dynamicTarget = indirectTarget(transfer, context);
    } else if (transfer.flow == FlowKind::Return) {
        dynamicTarget = context.read(Register::PR);
    }

    if (transfer.flow == FlowKind::DirectCall || transfer.flow == FlowKind::IndirectCall) {
        context.write(Register::PR,
                      context.builder().constI32(
                          static_cast<std::int32_t>(continuationAddress(transfer))));
    }

    if (block.terminator->slot) {
        if (liftInstruction(*block.terminator->slot, context) != LiftResult::Success) {
            return failure(BlockLiftStatus::UnsupportedInstruction,
                           block.terminator->slot->address);
        }
    }

    switch (transfer.flow) {
    case FlowKind::DirectBranch:
        context.builder().branch(targets.taken);
        break;
    case FlowKind::ConditionalBranch:
        context.builder().branchIf(condition, targets.taken, targets.notTaken);
        break;
    case FlowKind::IndirectBranch:
        if (targets.taken) {
            context.builder().branch(targets.taken);
        } else {
            context.builder().branchIndirect(dynamicTarget);
        }
        break;
    case FlowKind::DirectCall:
        context.builder().call(targets.function, targets.returnSite);
        break;
    case FlowKind::IndirectCall:
        if (targets.function) {
            context.builder().call(targets.function, targets.returnSite);
        } else {
            context.builder().callIndirect(dynamicTarget, targets.returnSite);
        }
        break;
    case FlowKind::Return:
        context.builder().ret(dynamicTarget);
        break;
    case FlowKind::ExceptionReturn:
        context.builder().returnFromException();
        break;
    case FlowKind::Trap: {
        const auto vector = std::get<ImmediateOperand>(transfer.operand(0)).value;
        context.builder().trap(static_cast<std::uint8_t>(vector));
        break;
    }
    case FlowKind::Fallthrough:
        return failure(BlockLiftStatus::InvalidInstruction, transfer.address);
    }
    return {};
}

std::string_view blockLiftStatusName(BlockLiftStatus status) noexcept {
    switch (status) {
    case BlockLiftStatus::Success: return "success";
    case BlockLiftStatus::InvalidInstruction: return "invalid-instruction";
    case BlockLiftStatus::UnsupportedInstruction: return "unsupported-instruction";
    case BlockLiftStatus::MissingDelaySlot: return "missing-delay-slot";
    case BlockLiftStatus::InvalidDelaySlot: return "invalid-delay-slot";
    case BlockLiftStatus::MissingSuccessor: return "missing-successor";
    case BlockLiftStatus::MissingTarget: return "missing-target";
    }
    return "unknown";
}

} // namespace saturn::sh2
