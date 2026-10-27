#include "test_support.hpp"

#include "saturn/sh2/cfg.hpp"

#include <algorithm>
#include <vector>

namespace {

using namespace saturn;

bool hasEdge(const sh2::BasicBlock& block, core::Address target, sh2::EdgeKind kind) {
    return std::any_of(block.successors.begin(), block.successors.end(),
                       [=](const sh2::CFGEdge& edge) {
                           return edge.target == target && edge.kind == kind;
                       });
}

void discoversBlocksAndBindsDelaySlots() {
    // 0x100: BT 0x108; 0x102: ADD #1,R0; 0x104: BRA 0x10C; 0x106: NOP.
    // Two separate return blocks make all discovered paths terminate.
    const auto image = test::imageFromWords(0x100U, {
        0x8902U, 0x7001U, 0xA002U, 0x0009U,
        0x000BU, 0x0009U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});

    REQUIRE(cfg.blocks.size() == 4);
    const auto* first = cfg.blockAt(0x100U);
    const auto* fallthrough = cfg.blockAt(0x102U);
    const auto* taken = cfg.blockAt(0x108U);
    const auto* destination = cfg.blockAt(0x10CU);
    REQUIRE(first != nullptr && fallthrough != nullptr && taken != nullptr && destination != nullptr);

    REQUIRE(first->terminator.has_value());
    REQUIRE(first->terminator->transfer.opcode == sh2::Opcode::Bt);
    REQUIRE(!first->terminator->slot.has_value());
    REQUIRE(hasEdge(*first, 0x108U, sh2::EdgeKind::Taken));
    REQUIRE(hasEdge(*first, 0x102U, sh2::EdgeKind::NotTaken));

    REQUIRE(fallthrough->instructions.size() == 1);
    REQUIRE(fallthrough->instructions[0].opcode == sh2::Opcode::AddImm);
    REQUIRE(fallthrough->terminator.has_value());
    REQUIRE(fallthrough->terminator->transfer.opcode == sh2::Opcode::Bra);
    REQUIRE(fallthrough->terminator->slot.has_value());
    REQUIRE(fallthrough->terminator->slot->opcode == sh2::Opcode::Nop);
    REQUIRE(hasEdge(*fallthrough, 0x10CU, sh2::EdgeKind::Taken));
}

void treatsDirectCallsAsCandidatesAndKeepsReturnSite() {
    // BSR 0x208; NOP; RTS; NOP; RTS; NOP
    const auto image = test::imageFromWords(0x200U, {
        0xB002U, 0x0009U, 0x000BU, 0x0009U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x200U});
    const auto* caller = cfg.blockAt(0x200U);
    REQUIRE(caller != nullptr);
    REQUIRE(caller->terminator.has_value());
    REQUIRE(caller->terminator->transfer.opcode == sh2::Opcode::Bsr);
    REQUIRE(caller->terminator->slot.has_value());
    REQUIRE(hasEdge(*caller, 0x208U, sh2::EdgeKind::Call));
    REQUIRE(hasEdge(*caller, 0x204U, sh2::EdgeKind::ReturnSite));

    const auto candidate = std::find_if(
        cfg.functionCandidates.begin(), cfg.functionCandidates.end(),
        [](const sh2::FunctionCandidate& item) {
            return item.address == 0x208U && item.evidence == sh2::FunctionEvidence::DirectCall;
        });
    REQUIRE(candidate != cfg.functionCandidates.end());
}

void resolvesPcRelativeConstantPoolJump() {
    // MOV.L @(1,PC),R3 loads 0x110 from 0x108, then JMP R3 reaches 0x110.
    const auto image = test::imageFromWords(0x100U, {
        0xD301U, 0x432BU, 0x0009U, 0x0009U,
        0x0000U, 0x0110U, 0x0009U, 0x0009U,
        0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    const auto* source = cfg.blockAt(0x100U);
    REQUIRE(source != nullptr && source->terminator.has_value());
    REQUIRE(source->terminator->transfer.directTarget == 0x110U);
    REQUIRE(hasEdge(*source, 0x110U, sh2::EdgeKind::Indirect));
    REQUIRE(cfg.blockAt(0x110U) != nullptr);
}

void resolvesRegisterRelativeBranch() {
    // MOV #-8,R3; BRAF R3 computes 0x102 + 4 - 8 = 0xFE; NOP is the slot.
    const auto image = test::imageFromWords(0x100U, {
        0xE3F8U, 0x0323U, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    const auto* source = cfg.blockAt(0x100U);
    REQUIRE(source != nullptr && source->terminator.has_value());
    REQUIRE(source->terminator->transfer.opcode == sh2::Opcode::Braf);
    REQUIRE(source->terminator->transfer.directTarget == 0xFEU);
    REQUIRE(hasEdge(*source, 0xFEU, sh2::EdgeKind::Indirect));
}

void invalidatesConstantsAcrossSleep() {
    // The interrupt that wakes SLEEP may alter any GPR, so R3 is no longer a
    // safe constant when the following JMP is analyzed.
    const auto image = test::imageFromWords(0x100U, {
        0xE308U, 0x001BU, 0x432BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    const auto* source = cfg.blockAt(0x100U);
    REQUIRE(source != nullptr && source->terminator.has_value());
    REQUIRE(source->terminator->transfer.opcode == sh2::Opcode::Jmp);
    REQUIRE(!source->terminator->transfer.directTarget.has_value());
    REQUIRE(std::any_of(source->successors.begin(), source->successors.end(),
                        [](const sh2::CFGEdge& edge) {
                            return edge.kind == sh2::EdgeKind::Indirect && !edge.target;
                        }));
}

} // namespace

void registerCfgTests(test::Registry& registry) {
    registry.add("cfg: recursive blocks and delay slots", discoversBlocksAndBindsDelaySlots);
    registry.add("cfg: direct call candidates", treatsDirectCallsAsCandidatesAndKeepsReturnSite);
    registry.add("cfg: resolves PC-relative constant-pool jump", resolvesPcRelativeConstantPoolJump);
    registry.add("cfg: resolves register-relative branch", resolvesRegisterRelativeBranch);
    registry.add("cfg: invalidates constants across sleep", invalidatesConstantsAcrossSleep);
}
