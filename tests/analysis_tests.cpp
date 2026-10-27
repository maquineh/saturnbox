#include "test_support.hpp"

#include "saturn/sh2/analysis.hpp"

namespace {

using namespace saturn;

void reportsUnsupportedAndUnresolvedReachableInstructions() {
    // BT/S 0x108 has an invalid delay slot. Its fallthrough contains an
    // unresolved JMP R3, while the taken block starts with another invalid word.
    const auto image = test::imageFromWords(0x100U, {
        0x8D02U, 0xFFFFU, 0x432BU, 0x0009U, 0xFFFFU,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    const auto report = sh2::analyzeCFG(cfg);

    REQUIRE(report.basicBlockCount == 3);
    REQUIRE(report.instructionCount == 5);
    REQUIRE(report.unsupportedSites.size() == 2);
    REQUIRE(report.unsupportedOpcodes.size() == 1);
    REQUIRE(report.unsupportedOpcodes[0].raw == 0xFFFFU);
    REQUIRE(report.unsupportedOpcodes[0].count == 2);
    REQUIRE(report.unsupportedOpcodes[0].priority == sh2::AnalysisPriority::Critical);
    REQUIRE(report.interruptedBlockCount == 1);
    REQUIRE(report.invalidDelaySlotCount == 1);
    REQUIRE(report.unresolvedIndirectTransfers.size() == 1);
    REQUIRE(report.unresolvedIndirectTransfers[0].address == 0x104U);
}

void reportsUnsafeDelaySlots() {
    const auto missingSlotImage = test::imageFromWords(0x100U, {0xA000U});
    const auto missingSlotCfg =
        sh2::BlockBuilder{}.build(missingSlotImage, std::vector<core::Address>{0x100U});
    const auto missingSlotReport = sh2::analyzeCFG(missingSlotCfg);
    REQUIRE(missingSlotReport.missingDelaySlotCount == 1);
    REQUIRE(missingSlotReport.delaySlotIssues.size() == 1);
    REQUIRE(missingSlotReport.delaySlotIssues[0].kind == sh2::DelaySlotIssueKind::Missing);
    REQUIRE(!missingSlotReport.delaySlotIssues[0].slotAddress.has_value());

    // A control transfer in a delay slot is architecturally illegal and must
    // be surfaced even though both words decode individually.
    const auto nestedTransferImage = test::imageFromWords(0x100U, {
        0xA001U, 0xA000U, 0x0009U, 0x000BU, 0x0009U,
    });
    const auto nestedTransferCfg =
        sh2::BlockBuilder{}.build(nestedTransferImage, std::vector<core::Address>{0x100U});
    const auto nestedTransferReport = sh2::analyzeCFG(nestedTransferCfg);
    REQUIRE(nestedTransferReport.controlTransferDelaySlotCount == 1);
    REQUIRE(nestedTransferReport.delaySlotIssues.size() == 1);
    REQUIRE(nestedTransferReport.delaySlotIssues[0].kind ==
            sh2::DelaySlotIssueKind::ControlTransfer);
    REQUIRE(nestedTransferReport.delaySlotIssues[0].slotAddress == 0x102U);
}

void reportsSirLifterCoverage() {
    // MOV #1,R0; ADD #1,R0; RTS; NOP (delay slot).
    const auto image = test::imageFromWords(0x100U, {
        0xE001U, 0x7001U, 0x000BU, 0x0009U,
    });
    const auto cfg = sh2::BlockBuilder{}.build(image, std::vector<core::Address>{0x100U});
    const auto report = sh2::analyzeCFG(cfg);
    REQUIRE(report.instructionCount == 4);
    REQUIRE(report.fallthroughInstructionCount == 3);
    REQUIRE(report.liftableInstructionCount == 3);
}

} // namespace

void registerAnalysisTests(test::Registry& registry) {
    registry.add("analysis: unsupported opcodes and indirect transfers",
                 reportsUnsupportedAndUnresolvedReachableInstructions);
    registry.add("analysis: unsafe delay slots", reportsUnsafeDelaySlots);
    registry.add("analysis: SIR lifter coverage", reportsSirLifterCoverage);
}
