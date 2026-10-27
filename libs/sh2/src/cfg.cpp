#include "saturn/sh2/cfg.hpp"

#include "saturn/sh2/format.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace saturn::sh2 {
namespace {

using Constants = std::array<std::optional<Address>, 16>;

[[nodiscard]] std::optional<std::size_t> generalRegisterIndex(Register reg) noexcept {
    const auto index = static_cast<std::size_t>(reg);
    return index < 16 ? std::optional{index} : std::nullopt;
}

void clearRegister(Constants& constants, Register reg) {
    if (const auto index = generalRegisterIndex(reg)) {
        constants[*index].reset();
    }
}

[[nodiscard]] Register registerOperand(const DecodedInstruction& instruction,
                                       std::size_t index) {
    return std::get<RegisterOperand>(instruction.operand(index)).reg;
}

void updateConstants(const DecodedInstruction& instruction,
                     const core::MemoryMap& image,
                     Constants& constants) {
    const auto copyConstant = [&](Register source, Register destination) {
        const auto sourceIndex = generalRegisterIndex(source);
        const auto destinationIndex = generalRegisterIndex(destination);
        if (destinationIndex) {
            constants[*destinationIndex] = sourceIndex ? constants[*sourceIndex] : std::nullopt;
        }
    };

    switch (instruction.opcode) {
    case Opcode::MovLongPcRelative: {
        const auto address = std::get<AddressOperand>(instruction.operand(0)).address;
        const auto destination = registerOperand(instruction, 1);
        if (const auto index = generalRegisterIndex(destination)) {
            constants[*index] = image.read32BE(address);
        }
        break;
    }
    case Opcode::Mova: {
        const auto destination = registerOperand(instruction, 1);
        if (const auto index = generalRegisterIndex(destination)) {
            constants[*index] = std::get<AddressOperand>(instruction.operand(0)).address;
        }
        break;
    }
    case Opcode::MovImm: {
        const auto destination = registerOperand(instruction, 1);
        if (const auto index = generalRegisterIndex(destination)) {
            constants[*index] = static_cast<Address>(
                std::get<ImmediateOperand>(instruction.operand(0)).value);
        }
        break;
    }
    case Opcode::MovReg:
        copyConstant(registerOperand(instruction, 0), registerOperand(instruction, 1));
        break;
    case Opcode::AddImm: {
        const auto destination = registerOperand(instruction, 1);
        if (const auto index = generalRegisterIndex(destination)) {
            if (constants[*index]) {
                *constants[*index] = static_cast<Address>(
                    *constants[*index] + std::get<ImmediateOperand>(instruction.operand(0)).value);
            }
        }
        break;
    }
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
    case Opcode::MovWordPcRelative:
    case Opcode::Not:
    case Opcode::Add:
    case Opcode::AddCarry:
    case Opcode::AddOverflow:
    case Opcode::Sub:
    case Opcode::SubCarry:
    case Opcode::SubOverflow:
    case Opcode::Div1:
    case Opcode::Neg:
    case Opcode::NegCarry:
    case Opcode::ExtSignedByte:
    case Opcode::ExtSignedWord:
    case Opcode::ExtUnsignedByte:
    case Opcode::ExtUnsignedWord:
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
        clearRegister(constants, registerOperand(instruction, 1));
        if (instruction.opcode == Opcode::MovBytePostIncrement ||
            instruction.opcode == Opcode::MovWordPostIncrement ||
            instruction.opcode == Opcode::MovLongPostIncrement) {
            clearRegister(constants, std::get<MemoryOperand>(instruction.operand(0)).base);
        }
        break;
    case Opcode::MovBytePreDecrement:
    case Opcode::MovWordPreDecrement:
    case Opcode::MovLongPreDecrement:
    case Opcode::StsPrPreDecrement:
        clearRegister(constants, std::get<MemoryOperand>(instruction.operand(1)).base);
        break;
    case Opcode::MovT:
    case Opcode::Dt:
    case Opcode::Shll:
    case Opcode::Shlr:
    case Opcode::Shal:
    case Opcode::Shar:
    case Opcode::Rotl:
    case Opcode::Rotr:
    case Opcode::Rotcl:
    case Opcode::Rotcr:
    case Opcode::Shll2:
    case Opcode::Shll8:
    case Opcode::Shll16:
    case Opcode::Shlr2:
    case Opcode::Shlr8:
    case Opcode::Shlr16:
        clearRegister(constants, registerOperand(instruction, 0));
        break;
    case Opcode::Stc:
    case Opcode::Sts:
        clearRegister(constants, registerOperand(instruction, 1));
        break;
    case Opcode::AndImm:
    case Opcode::OrImm:
    case Opcode::XorImm:
        clearRegister(constants, registerOperand(instruction, 1));
        break;
    case Opcode::LdsPrPostIncrement:
        clearRegister(constants, std::get<MemoryOperand>(instruction.operand(0)).base);
        break;
    case Opcode::Sleep:
        // Execution resumes after an interrupt handler, which may have changed
        // any general register. Do not propagate pre-sleep values past it.
        constants.fill(std::nullopt);
        break;
    case Opcode::Invalid:
    case Opcode::MovByteStore:
    case Opcode::MovWordStore:
    case Opcode::MovLongStore:
    case Opcode::MovByteIndexedStore:
    case Opcode::MovWordIndexedStore:
    case Opcode::MovLongIndexedStore:
    case Opcode::MovByteDisplacementStore:
    case Opcode::MovWordDisplacementStore:
    case Opcode::MovLongDisplacementStore:
    case Opcode::CmpEq:
    case Opcode::CmpHs:
    case Opcode::CmpGe:
    case Opcode::CmpHi:
    case Opcode::CmpGt:
    case Opcode::CmpEqImm:
    case Opcode::CmpPz:
    case Opcode::CmpPl:
    case Opcode::Tst:
    case Opcode::TstImm:
    case Opcode::MulLong:
    case Opcode::MulSignedWord:
    case Opcode::MulUnsignedWord:
    case Opcode::Div0Signed:
    case Opcode::Div0Unsigned:
    case Opcode::Bra:
    case Opcode::Braf:
    case Opcode::Bt:
    case Opcode::Bf:
    case Opcode::BtS:
    case Opcode::BfS:
    case Opcode::Bsr:
    case Opcode::Bsrf:
    case Opcode::Jmp:
    case Opcode::Jsr:
    case Opcode::Rts:
    case Opcode::Rte:
    case Opcode::Nop:
    case Opcode::Trapa:
    case Opcode::Ldc:
    case Opcode::Lds:
        break;
    }
}

void resolveIndirectTarget(DecodedInstruction& instruction, const Constants& constants) {
    if (instruction.flow != FlowKind::IndirectBranch &&
        instruction.flow != FlowKind::IndirectCall) {
        return;
    }
    const auto source = registerOperand(instruction, 0);
    if (const auto index = generalRegisterIndex(source)) {
        if (constants[*index] &&
            (instruction.opcode == Opcode::Braf || instruction.opcode == Opcode::Bsrf)) {
            instruction.directTarget = static_cast<Address>(
                instruction.address + 4U + *constants[*index]);
        } else {
            instruction.directTarget = constants[*index];
        }
    }
}

[[nodiscard]] bool isTerminator(const DecodedInstruction& instruction) noexcept {
    return !instruction.valid || instruction.flow != FlowKind::Fallthrough;
}

[[nodiscard]] Address nextAddress(const DecodedInstruction& instruction) noexcept {
    return static_cast<Address>(instruction.address + instruction.length);
}

[[nodiscard]] Address continuationAddress(const DecodedInstruction& instruction) noexcept {
    return static_cast<Address>(instruction.address + instruction.length +
                                (instruction.delaySlot == DelaySlot::None ? 0 : 2));
}

void addSuccessors(const DecodedInstruction& instruction, std::vector<CFGEdge>& successors) {
    const auto direct = instruction.directTarget;
    switch (instruction.flow) {
    case FlowKind::DirectBranch:
        if (direct) {
            successors.push_back({direct, EdgeKind::Taken});
        }
        break;
    case FlowKind::ConditionalBranch:
        if (direct) {
            successors.push_back({direct, EdgeKind::Taken});
        }
        successors.push_back({continuationAddress(instruction), EdgeKind::NotTaken});
        break;
    case FlowKind::DirectCall:
        if (direct) {
            successors.push_back({direct, EdgeKind::Call});
        }
        successors.push_back({continuationAddress(instruction), EdgeKind::ReturnSite});
        break;
    case FlowKind::IndirectCall:
        successors.push_back({direct, EdgeKind::Indirect});
        successors.push_back({continuationAddress(instruction), EdgeKind::ReturnSite});
        break;
    case FlowKind::IndirectBranch:
        successors.push_back({direct, EdgeKind::Indirect});
        break;
    case FlowKind::Fallthrough:
    case FlowKind::Return:
    case FlowKind::ExceptionReturn:
    case FlowKind::Trap:
        break;
    }
}

void emitDiscoveryTargets(const DecodedInstruction& instruction,
                          const core::MemoryMap& image,
                          std::set<Address>& worklist,
                          std::set<Address>& leaders,
                          std::map<Address, FunctionCandidate>& candidates) {
    const auto enqueue = [&](Address target) {
        if (image.contains(target, 2)) {
            worklist.insert(target);
            leaders.insert(target);
        }
    };

    switch (instruction.flow) {
    case FlowKind::DirectBranch:
        if (instruction.directTarget) {
            enqueue(*instruction.directTarget);
        }
        break;
    case FlowKind::ConditionalBranch:
        if (instruction.directTarget) {
            enqueue(*instruction.directTarget);
        }
        enqueue(continuationAddress(instruction));
        break;
    case FlowKind::DirectCall:
        if (instruction.directTarget) {
            enqueue(*instruction.directTarget);
            candidates.insert_or_assign(*instruction.directTarget,
                                        FunctionCandidate{*instruction.directTarget,
                                                          Confidence::High,
                                                          FunctionEvidence::DirectCall});
        }
        enqueue(continuationAddress(instruction));
        break;
    case FlowKind::IndirectCall:
        if (instruction.directTarget) {
            enqueue(*instruction.directTarget);
        }
        enqueue(continuationAddress(instruction));
        break;
    case FlowKind::IndirectBranch:
        if (instruction.directTarget) {
            enqueue(*instruction.directTarget);
        }
        break;
    case FlowKind::Return:
    case FlowKind::ExceptionReturn:
    case FlowKind::Trap:
    case FlowKind::Fallthrough:
        break;
    }
}

[[nodiscard]] std::string dotNodeId(Address address) {
    std::ostringstream output;
    output << "b_" << std::hex << address;
    return output.str();
}

[[nodiscard]] std::string edgeName(EdgeKind kind) {
    switch (kind) {
    case EdgeKind::Fallthrough: return "fallthrough";
    case EdgeKind::Taken: return "taken";
    case EdgeKind::NotTaken: return "not taken";
    case EdgeKind::Call: return "call";
    case EdgeKind::ReturnSite: return "return site";
    case EdgeKind::Indirect: return "indirect";
    }
    return "unknown";
}

} // namespace

const BasicBlock* CFG::blockAt(Address address) const noexcept {
    const auto found = blocks.find(address);
    return found == blocks.end() ? nullptr : &found->second;
}

BlockBuilder::BlockBuilder(Decoder decoder) : decoder_(std::move(decoder)) {}

CFG BlockBuilder::build(const core::MemoryMap& image, std::span<const Address> entries) const {
    std::map<Address, DecodedInstruction> decoded;
    std::set<Address> worklist;
    std::set<Address> leaders;
    std::map<Address, FunctionCandidate> candidates;

    for (const auto entry : entries) {
        if (image.contains(entry, 2)) {
            worklist.insert(entry);
            leaders.insert(entry);
            candidates.insert_or_assign(entry,
                                        FunctionCandidate{entry, Confidence::Proven,
                                                          FunctionEvidence::EntryPoint});
        }
    }

    // Recursive descent follows only executable, concrete edges. Decoding a
    // delay slot does not make it an independent successor of the transfer.
    while (!worklist.empty()) {
        const auto start = *worklist.begin();
        worklist.erase(worklist.begin());
        Address pc = start;
        Constants constants{};

        while (image.contains(pc, 2) && !decoded.contains(pc)) {
            const auto word = image.read16BE(pc);
            auto instruction = decoder_.decode(pc, *word).instruction;
            resolveIndirectTarget(instruction, constants);
            decoded.emplace(pc, instruction);

            if (isTerminator(instruction)) {
                if (instruction.valid && instruction.delaySlot != DelaySlot::None) {
                    const auto slotAddress = nextAddress(instruction);
                    if (image.contains(slotAddress, 2) && !decoded.contains(slotAddress)) {
                        const auto slotWord = image.read16BE(slotAddress);
                        decoded.emplace(slotAddress, decoder_.decode(slotAddress, *slotWord).instruction);
                    }
                }
                emitDiscoveryTargets(instruction, image, worklist, leaders, candidates);
                break;
            }
            updateConstants(instruction, image, constants);
            pc = nextAddress(instruction);
        }
    }

    CFG cfg;
    cfg.functionCandidates.reserve(candidates.size());
    for (const auto& [_, candidate] : candidates) {
        cfg.functionCandidates.push_back(candidate);
    }

    // The complete leader set is known now, so this phase safely splits a
    // linear sequence if a later-discovered branch targeted its interior.
    for (const auto start : leaders) {
        const auto first = decoded.find(start);
        if (first == decoded.end()) {
            continue;
        }

        BasicBlock block;
        block.start = start;
        Address pc = start;
        while (true) {
            if (pc != start && leaders.contains(pc)) {
                block.successors.push_back({pc, EdgeKind::Fallthrough});
                break;
            }

            const auto found = decoded.find(pc);
            if (found == decoded.end()) {
                break;
            }
            const auto& instruction = found->second;

            if (!isTerminator(instruction)) {
                block.instructions.push_back(instruction);
                pc = nextAddress(instruction);
                continue;
            }

            DelayedTerminator terminator{instruction, std::nullopt};
            if (instruction.valid && instruction.delaySlot != DelaySlot::None) {
                const auto slot = decoded.find(nextAddress(instruction));
                if (slot != decoded.end()) {
                    terminator.slot = slot->second;
                }
            }
            block.terminator = std::move(terminator);
            if (instruction.valid) {
                addSuccessors(instruction, block.successors);
            }
            break;
        }
        cfg.blocks.emplace(start, std::move(block));
    }

    return cfg;
}

std::string toDot(const CFG& cfg) {
    std::ostringstream output;
    output << "digraph sh2_cfg {\n"
           << "  rankdir=TB;\n"
           << "  node [shape=box, fontname=\"monospace\"];\n";

    for (const auto& [address, block] : cfg.blocks) {
        output << "  " << dotNodeId(address) << " [label=\"" << formatAddress(address) << ":\\l";
        for (const auto& instruction : block.instructions) {
            output << formatInstruction(instruction) << "\\l";
        }
        if (block.terminator) {
            output << formatInstruction(block.terminator->transfer) << "\\l";
            if (block.terminator->slot) {
                output << "  [delay] " << formatInstruction(*block.terminator->slot) << "\\l";
            }
        }
        output << "\"];\n";
    }

    std::set<Address> externalTargets;
    for (const auto& [_, block] : cfg.blocks) {
        for (const auto& edge : block.successors) {
            if (edge.target && !cfg.blocks.contains(*edge.target)) {
                externalTargets.insert(*edge.target);
            }
        }
    }
    for (const auto target : externalTargets) {
        output << "  " << dotNodeId(target) << " [label=\"" << formatAddress(target)
               << "\\nexternal target\", shape=ellipse, style=dashed];\n";
    }

    for (const auto& [address, block] : cfg.blocks) {
        for (const auto& edge : block.successors) {
            if (!edge.target) {
                output << "  " << dotNodeId(address) << " -> indirect [label=\""
                       << edgeName(edge.kind) << "\"];\n";
                continue;
            }
            output << "  " << dotNodeId(address) << " -> " << dotNodeId(*edge.target)
                   << " [label=\"" << edgeName(edge.kind) << "\"];\n";
        }
    }
    output << "}\n";
    return output.str();
}

} // namespace saturn::sh2
