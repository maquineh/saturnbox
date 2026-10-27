#include "test_support.hpp"

#include "saturn/sh2/decoder.hpp"

#include <variant>

namespace {

using namespace saturn::sh2;

void decodesDataAndArithmeticSubset() {
    const Decoder decoder;

    const auto movImmediate = decoder.decode(0x06004000U, 0xE3FCU).instruction;
    REQUIRE(movImmediate.valid);
    REQUIRE(movImmediate.opcode == Opcode::MovImm);
    REQUIRE(movImmediate.flow == FlowKind::Fallthrough);
    REQUIRE(std::get<ImmediateOperand>(movImmediate.operand(0)).value == -4);
    REQUIRE(std::get<RegisterOperand>(movImmediate.operand(1)).reg == Register::R3);

    const auto movRegister = decoder.decode(0x06004002U, 0x6213U).instruction;
    REQUIRE(movRegister.opcode == Opcode::MovReg);
    REQUIRE(std::get<RegisterOperand>(movRegister.operand(0)).reg == Register::R1);
    REQUIRE(std::get<RegisterOperand>(movRegister.operand(1)).reg == Register::R2);

    const auto load = decoder.decode(0x06004004U, 0x6342U).instruction;
    REQUIRE(load.opcode == Opcode::MovLongLoad);
    const auto& loadAddress = std::get<MemoryOperand>(load.operand(0));
    REQUIRE(loadAddress.base == Register::R4);
    REQUIRE(loadAddress.width == 4);
    REQUIRE(std::get<RegisterOperand>(load.operand(1)).reg == Register::R3);

    const auto store = decoder.decode(0x06004006U, 0x2541U).instruction;
    REQUIRE(store.opcode == Opcode::MovWordStore);
    REQUIRE(std::get<RegisterOperand>(store.operand(0)).reg == Register::R4);
    REQUIRE(std::get<MemoryOperand>(store.operand(1)).base == Register::R5);
    REQUIRE(std::get<MemoryOperand>(store.operand(1)).width == 2);

    const auto add = decoder.decode(0x06004008U, 0x321CU).instruction;
    const auto sub = decoder.decode(0x0600400AU, 0x3218U).instruction;
    REQUIRE(add.opcode == Opcode::Add);
    REQUIRE(sub.opcode == Opcode::Sub);

    const auto logicalOpcodes = std::vector<std::pair<std::uint16_t, Opcode>>{
        {static_cast<std::uint16_t>(0x2128U), Opcode::Tst},
        {static_cast<std::uint16_t>(0x2129U), Opcode::And},
        {static_cast<std::uint16_t>(0x212AU), Opcode::Xor},
        {static_cast<std::uint16_t>(0x212BU), Opcode::Or},
        {static_cast<std::uint16_t>(0x6127U), Opcode::Not},
    };
    for (const auto& [word, opcode] : logicalOpcodes) {
        REQUIRE(decoder.decode(0x0600400CU, word).instruction.opcode == opcode);
    }
}

void calculatesPcRelativeBranchTargets() {
    const Decoder decoder;

    const auto bra = decoder.decode(0x06004000U, 0xA00AU).instruction;
    REQUIRE(bra.opcode == Opcode::Bra);
    REQUIRE(bra.directTarget == 0x06004018U);
    REQUIRE(bra.flow == FlowKind::DirectBranch);
    REQUIRE(bra.delaySlot == DelaySlot::Always);

    const auto backwardsBsr = decoder.decode(0x00000100U, 0xBFFFU).instruction;
    REQUIRE(backwardsBsr.directTarget == 0x00000102U);

    const auto bt = decoder.decode(0x00001000U, 0x89FEU).instruction;
    REQUIRE(bt.directTarget == 0x00001000U);
    REQUIRE(bt.flow == FlowKind::ConditionalBranch);
    REQUIRE(bt.delaySlot == DelaySlot::None);

    const auto delayed = decoder.decode(0x00000200U, 0x8D01U).instruction;
    REQUIRE(delayed.opcode == Opcode::BtS);
    REQUIRE(delayed.directTarget == 0x00000206U);
    // SH-2 executes the following instruction on either BT/S path.
    REQUIRE(delayed.delaySlot == DelaySlot::Always);
}

void classifiesControlTransfersAndUnknownWords() {
    const Decoder decoder;
    const auto jmp = decoder.decode(0x06004000U, 0x472BU).instruction;
    const auto jsr = decoder.decode(0x06004000U, 0x4A0BU).instruction;
    const auto rts = decoder.decode(0x06004000U, 0x000BU).instruction;
    const auto rte = decoder.decode(0x06004000U, 0x002BU).instruction;
    const auto unknown = decoder.decode(0x06004000U, 0xFFFFU).instruction;

    REQUIRE(jmp.opcode == Opcode::Jmp && jmp.flow == FlowKind::IndirectBranch);
    REQUIRE(std::get<RegisterOperand>(jmp.operand(0)).reg == Register::R7);
    REQUIRE(jsr.opcode == Opcode::Jsr && jsr.flow == FlowKind::IndirectCall);
    REQUIRE(rts.flow == FlowKind::Return && rts.delaySlot == DelaySlot::Always);
    REQUIRE(rte.flow == FlowKind::ExceptionReturn && rte.delaySlot == DelaySlot::Always);
    REQUIRE(!unknown.valid && unknown.opcode == Opcode::Invalid);
}

void decodesAddressingVariantsComparisonsAndShifts() {
    const Decoder decoder;

    const auto post = decoder.decode(0x06004000U, 0x6346U).instruction;
    REQUIRE(post.opcode == Opcode::MovLongPostIncrement);
    REQUIRE(std::get<MemoryOperand>(post.operand(0)).mode == MemoryOperand::Mode::PostIncrement);
    REQUIRE(std::get<MemoryOperand>(post.operand(0)).width == 4);

    const auto pre = decoder.decode(0x06004002U, 0x2546U).instruction;
    REQUIRE(pre.opcode == Opcode::MovLongPreDecrement);
    REQUIRE(std::get<MemoryOperand>(pre.operand(1)).mode == MemoryOperand::Mode::PreDecrement);

    const auto movt = decoder.decode(0x06004004U, 0x0129U).instruction;
    REQUIRE(movt.opcode == Opcode::MovT);
    REQUIRE(std::get<RegisterOperand>(movt.operand(0)).reg == Register::R1);

    const auto mova = decoder.decode(0x06004006U, 0xC702U).instruction;
    REQUIRE(mova.opcode == Opcode::Mova);
    REQUIRE(std::get<AddressOperand>(mova.operand(0)).address == 0x06004010U);
    REQUIRE(std::get<RegisterOperand>(mova.operand(1)).reg == Register::R0);

    const auto cmpPz = decoder.decode(0x06004008U, 0x4111U).instruction;
    const auto cmpPl = decoder.decode(0x06004008U, 0x4515U).instruction;
    REQUIRE(cmpPz.opcode == Opcode::CmpPz);
    REQUIRE(std::get<RegisterOperand>(cmpPz.operand(0)).reg == Register::R1);
    REQUIRE(cmpPl.opcode == Opcode::CmpPl);

    const auto shift = decoder.decode(0x0600400AU, 0x4420U).instruction;
    const auto rotate = decoder.decode(0x0600400CU, 0x4405U).instruction;
    REQUIRE(shift.opcode == Opcode::Shal);
    REQUIRE(rotate.opcode == Opcode::Rotr);
}

void decodesBootProgramInstructions() {
    const Decoder decoder;

    const auto movLong = decoder.decode(0x06002E40U, 0xD708U).instruction;
    REQUIRE(movLong.opcode == Opcode::MovLongPcRelative);
    REQUIRE(std::get<AddressOperand>(movLong.operand(0)).address == 0x06002E64U);
    REQUIRE(std::get<RegisterOperand>(movLong.operand(1)).reg == Register::R7);

    const auto movWord = decoder.decode(0x06002E8AU, 0x9010U).instruction;
    REQUIRE(movWord.opcode == Opcode::MovWordPcRelative);
    REQUIRE(std::get<AddressOperand>(movWord.operand(0)).address == 0x06002EAEU);

    const auto dt = decoder.decode(0x06002E54U, 0x4210U).instruction;
    REQUIRE(dt.opcode == Opcode::Dt);
    REQUIRE(std::get<RegisterOperand>(dt.operand(0)).reg == Register::R2);

    const auto tst = decoder.decode(0x06002ECAU, 0xC801U).instruction;
    REQUIRE(tst.opcode == Opcode::TstImm);
    REQUIRE(std::get<ImmediateOperand>(tst.operand(0)).value == 1);
    REQUIRE(!std::get<ImmediateOperand>(tst.operand(0)).signExtended);

    const auto savePr = decoder.decode(0x06002EC4U, 0x4F22U).instruction;
    REQUIRE(savePr.opcode == Opcode::StsPrPreDecrement);
    REQUIRE(std::get<RegisterOperand>(savePr.operand(0)).reg == Register::PR);
    REQUIRE(std::get<MemoryOperand>(savePr.operand(1)).base == Register::R15);
    REQUIRE(std::get<MemoryOperand>(savePr.operand(1)).mode == MemoryOperand::Mode::PreDecrement);

    const auto restorePr = decoder.decode(0x06002F58U, 0x4F26U).instruction;
    REQUIRE(restorePr.opcode == Opcode::LdsPrPostIncrement);
    REQUIRE(std::get<MemoryOperand>(restorePr.operand(0)).mode == MemoryOperand::Mode::PostIncrement);
    REQUIRE(std::get<RegisterOperand>(restorePr.operand(1)).reg == Register::PR);

    const auto saveMacl = decoder.decode(0x06030854U, 0x4F12U).instruction;
    REQUIRE(saveMacl.opcode == Opcode::StsPrPreDecrement);
    REQUIRE(std::get<RegisterOperand>(saveMacl.operand(0)).reg == Register::MACL);
    REQUIRE(std::get<MemoryOperand>(saveMacl.operand(1)).base == Register::R15);

    const auto restoreMach = decoder.decode(0x06030860U, 0x4F06U).instruction;
    REQUIRE(restoreMach.opcode == Opcode::LdsPrPostIncrement);
    REQUIRE(std::get<MemoryOperand>(restoreMach.operand(0)).base == Register::R15);
    REQUIRE(std::get<RegisterOperand>(restoreMach.operand(1)).reg == Register::MACH);
}

void decodesExtendedM0ArithmeticAndSystemInstructions() {
    const Decoder decoder;

    const auto arithmeticOpcodes = std::vector<std::pair<std::uint16_t, Opcode>>{
        {static_cast<std::uint16_t>(0x321EU), Opcode::AddCarry},
        {static_cast<std::uint16_t>(0x321FU), Opcode::AddOverflow},
        {static_cast<std::uint16_t>(0x321AU), Opcode::SubCarry},
        {static_cast<std::uint16_t>(0x321BU), Opcode::SubOverflow},
        {static_cast<std::uint16_t>(0x621BU), Opcode::Neg},
        {static_cast<std::uint16_t>(0x621AU), Opcode::NegCarry},
        {static_cast<std::uint16_t>(0x0217U), Opcode::MulLong},
        {static_cast<std::uint16_t>(0x221FU), Opcode::MulSignedWord},
        {static_cast<std::uint16_t>(0x221EU), Opcode::MulUnsignedWord},
    };
    for (const auto& [word, opcode] : arithmeticOpcodes) {
        const auto instruction = decoder.decode(0x06004000U, word).instruction;
        REQUIRE(instruction.opcode == opcode);
        REQUIRE(std::get<RegisterOperand>(instruction.operand(0)).reg == Register::R1);
        REQUIRE(std::get<RegisterOperand>(instruction.operand(1)).reg == Register::R2);
    }

    const auto braf = decoder.decode(0x06004000U, 0x0323U).instruction;
    REQUIRE(braf.opcode == Opcode::Braf);
    REQUIRE(braf.flow == FlowKind::IndirectBranch);
    REQUIRE(braf.delaySlot == DelaySlot::Always);
    REQUIRE(std::get<RegisterOperand>(braf.operand(0)).reg == Register::R3);

    const auto bsrf = decoder.decode(0x06004000U, 0x0403U).instruction;
    REQUIRE(bsrf.opcode == Opcode::Bsrf);
    REQUIRE(bsrf.flow == FlowKind::IndirectCall);
    REQUIRE(std::get<RegisterOperand>(bsrf.operand(0)).reg == Register::R4);

    const auto trapa = decoder.decode(0x06004000U, 0xC37FU).instruction;
    REQUIRE(trapa.opcode == Opcode::Trapa);
    REQUIRE(trapa.flow == FlowKind::Trap);
    REQUIRE(std::get<ImmediateOperand>(trapa.operand(0)).value == 0x7F);
    REQUIRE(!std::get<ImmediateOperand>(trapa.operand(0)).signExtended);

    const auto sleep = decoder.decode(0x06004000U, 0x001BU).instruction;
    REQUIRE(sleep.opcode == Opcode::Sleep);
    REQUIRE(sleep.flow == FlowKind::Fallthrough);
}

void decodesRockmanX4AddressingForms() {
    const Decoder decoder;

    const auto displacedWordStore = decoder.decode(0x06003038U, 0x81EAU).instruction;
    REQUIRE(displacedWordStore.opcode == Opcode::MovWordDisplacementStore);
    REQUIRE(std::get<RegisterOperand>(displacedWordStore.operand(0)).reg == Register::R0);
    const auto& wordDestination = std::get<MemoryOperand>(displacedWordStore.operand(1));
    REQUIRE(wordDestination.base == Register::R14);
    REQUIRE(wordDestination.displacement == 20);
    REQUIRE(wordDestination.width == 2);

    const auto displacedLongStore = decoder.decode(0x060100DCU, 0x1751U).instruction;
    REQUIRE(displacedLongStore.opcode == Opcode::MovLongDisplacementStore);
    REQUIRE(std::get<RegisterOperand>(displacedLongStore.operand(0)).reg == Register::R5);
    const auto& longDestination = std::get<MemoryOperand>(displacedLongStore.operand(1));
    REQUIRE(longDestination.base == Register::R7);
    REQUIRE(longDestination.displacement == 4);

    const auto indexedStore = decoder.decode(0x06003046U, 0x0E25U).instruction;
    REQUIRE(indexedStore.opcode == Opcode::MovWordIndexedStore);
    REQUIRE(std::get<RegisterOperand>(indexedStore.operand(0)).reg == Register::R2);
    const auto& indexedDestination = std::get<MemoryOperand>(indexedStore.operand(1));
    REQUIRE(indexedDestination.base == Register::R14);
    REQUIRE(indexedDestination.index == Register::R0);

    const auto indexedLoad = decoder.decode(0x06010376U, 0x000DU).instruction;
    REQUIRE(indexedLoad.opcode == Opcode::MovWordIndexedLoad);
    const auto& indexedSource = std::get<MemoryOperand>(indexedLoad.operand(0));
    REQUIRE(indexedSource.base == Register::R0);
    REQUIRE(indexedSource.index == Register::R0);
    REQUIRE(std::get<RegisterOperand>(indexedLoad.operand(1)).reg == Register::R0);

    const auto zeroExtend = decoder.decode(0x06010376U, 0x600DU).instruction;
    REQUIRE(zeroExtend.opcode == Opcode::ExtUnsignedWord);
    REQUIRE(std::get<RegisterOperand>(zeroExtend.operand(0)).reg == Register::R0);
    REQUIRE(std::get<RegisterOperand>(zeroExtend.operand(1)).reg == Register::R0);

    const auto shift = decoder.decode(0x0601018AU, 0x4108U).instruction;
    REQUIRE(shift.opcode == Opcode::Shll2);
    REQUIRE(std::get<RegisterOperand>(shift.operand(0)).reg == Register::R1);

    const auto stc = decoder.decode(0x06010526U, 0x0002U).instruction;
    REQUIRE(stc.opcode == Opcode::Stc);
    REQUIRE(std::get<RegisterOperand>(stc.operand(0)).reg == Register::SR);
    REQUIRE(std::get<RegisterOperand>(stc.operand(1)).reg == Register::R0);

    const auto stcVbr = decoder.decode(0x0601010AU, 0x0422U).instruction;
    REQUIRE(stcVbr.opcode == Opcode::Stc);
    REQUIRE(std::get<RegisterOperand>(stcVbr.operand(0)).reg == Register::VBR);
    REQUIRE(std::get<RegisterOperand>(stcVbr.operand(1)).reg == Register::R4);

    const auto immediateOr = decoder.decode(0x0601052CU, 0xCBF0U).instruction;
    REQUIRE(immediateOr.opcode == Opcode::OrImm);
    REQUIRE(std::get<ImmediateOperand>(immediateOr.operand(0)).value == 0xF0);
    REQUIRE(std::get<RegisterOperand>(immediateOr.operand(1)).reg == Register::R0);

    const auto stsMacl = decoder.decode(0x060345EAU, 0x031AU).instruction;
    REQUIRE(stsMacl.opcode == Opcode::Sts);
    REQUIRE(std::get<RegisterOperand>(stsMacl.operand(0)).reg == Register::MACL);
    REQUIRE(std::get<RegisterOperand>(stsMacl.operand(1)).reg == Register::R3);

    const auto ldcSr = decoder.decode(0x0601052EU, 0x400EU).instruction;
    REQUIRE(ldcSr.opcode == Opcode::Ldc);
    REQUIRE(std::get<RegisterOperand>(ldcSr.operand(0)).reg == Register::R0);
    REQUIRE(std::get<RegisterOperand>(ldcSr.operand(1)).reg == Register::SR);

    const auto div0s = decoder.decode(0x0602FDB8U, 0x2127U).instruction;
    REQUIRE(div0s.opcode == Opcode::Div0Signed);
    REQUIRE(std::get<RegisterOperand>(div0s.operand(0)).reg == Register::R2);
    REQUIRE(std::get<RegisterOperand>(div0s.operand(1)).reg == Register::R1);

    const auto cmpStr = decoder.decode(0x06031E52U, 0x264CU).instruction;
    REQUIRE(cmpStr.opcode == Opcode::CmpStr);
    REQUIRE(std::get<RegisterOperand>(cmpStr.operand(0)).reg == Register::R4);
    REQUIRE(std::get<RegisterOperand>(cmpStr.operand(1)).reg == Register::R6);
}

} // namespace

void registerDecoderTests(test::Registry& registry) {
    registry.add("decoder: data/arithmetic subset", decodesDataAndArithmeticSubset);
    registry.add("decoder: PC-relative targets and slots", calculatesPcRelativeBranchTargets);
    registry.add("decoder: transfer classification", classifiesControlTransfersAndUnknownWords);
    registry.add("decoder: addressing, compare and shift formats", decodesAddressingVariantsComparisonsAndShifts);
    registry.add("decoder: Saturn boot program instructions", decodesBootProgramInstructions);
    registry.add("decoder: extended M0 arithmetic and system instructions",
                 decodesExtendedM0ArithmeticAndSystemInstructions);
    registry.add("decoder: Rockman X4 addressing forms", decodesRockmanX4AddressingForms);
}
