#include "saturn/sh2/analysis.hpp"
#include "saturn/sh2/lifter.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <utility>

namespace saturn::sh2 {
namespace {

[[nodiscard]] int priorityRank(AnalysisPriority priority) noexcept {
    switch (priority) {
    case AnalysisPriority::Critical: return 3;
    case AnalysisPriority::High: return 2;
    case AnalysisPriority::Medium: return 1;
    }
    return 0;
}

void recordUnsupported(std::map<Address, UnsupportedSite>& sites,
                       const DecodedInstruction& instruction,
                       Address block,
                       AnalysisPriority priority,
                       UnsupportedContext context) {
    if (instruction.valid) {
        return;
    }
    const UnsupportedSite candidate{
        instruction.address, block, instruction.raw, priority, context};
    const auto found = sites.find(instruction.address);
    if (found == sites.end() ||
        priorityRank(candidate.priority) > priorityRank(found->second.priority)) {
        sites.insert_or_assign(instruction.address, candidate);
    }
}

} // namespace

AnalysisReport analyzeCFG(const CFG& cfg) {
    AnalysisReport report;
    report.basicBlockCount = cfg.blocks.size();

    std::map<Address, const DecodedInstruction*> instructions;
    std::map<Address, UnsupportedSite> sites;

    for (const auto& [blockAddress, block] : cfg.blocks) {
        for (const auto& instruction : block.instructions) {
            instructions.try_emplace(instruction.address, &instruction);
            recordUnsupported(sites, instruction, blockAddress,
                              AnalysisPriority::Medium, UnsupportedContext::Instruction);
        }

        if (!block.terminator) {
            continue;
        }
        const auto& transfer = block.terminator->transfer;
        instructions.try_emplace(transfer.address, &transfer);
        if (!transfer.valid) {
            ++report.interruptedBlockCount;
            recordUnsupported(sites, transfer, blockAddress,
                              AnalysisPriority::High, UnsupportedContext::BlockTerminator);
        }

        if ((transfer.flow == FlowKind::IndirectBranch ||
             transfer.flow == FlowKind::IndirectCall) &&
            !transfer.directTarget) {
            report.unresolvedIndirectTransfers.push_back(
                {transfer.address, blockAddress, transfer.flow});
        }

        if (transfer.delaySlot == DelaySlot::None) {
            continue;
        }

        if (!block.terminator->slot) {
            ++report.missingDelaySlotCount;
            report.delaySlotIssues.push_back(
                {transfer.address, std::nullopt, blockAddress, DelaySlotIssueKind::Missing});
            continue;
        }

        const auto& slot = *block.terminator->slot;
        instructions.try_emplace(slot.address, &slot);
        if (!slot.valid) {
            ++report.invalidDelaySlotCount;
            report.delaySlotIssues.push_back(
                {transfer.address, slot.address, blockAddress, DelaySlotIssueKind::Unsupported});
            recordUnsupported(sites, slot, blockAddress,
                              AnalysisPriority::Critical, UnsupportedContext::DelaySlot);
        } else if (slot.flow != FlowKind::Fallthrough) {
            ++report.controlTransferDelaySlotCount;
            report.delaySlotIssues.push_back(
                {transfer.address, slot.address, blockAddress,
                 DelaySlotIssueKind::ControlTransfer});
        }
    }

    report.instructionCount = instructions.size();
    std::map<Opcode, UnliftedOpcodeSummary> unlifted;
    for (const auto& [_, instruction] : instructions) {
        if (!instruction->valid || instruction->flow != FlowKind::Fallthrough) {
            continue;
        }
        ++report.fallthroughInstructionCount;
        if (lifterFor(instruction->opcode)) {
            ++report.liftableInstructionCount;
        } else {
            auto [iterator, inserted] = unlifted.try_emplace(
                instruction->opcode,
                UnliftedOpcodeSummary{instruction->opcode, 0, {}});
            ++iterator->second.count;
            iterator->second.addresses.push_back(instruction->address);
        }
    }
    for (auto& [_, summary] : unlifted) {
        report.unliftedOpcodes.push_back(std::move(summary));
    }
    std::ranges::sort(report.unliftedOpcodes, [](const auto& left, const auto& right) {
        if (left.count != right.count) {
            return left.count > right.count;
        }
        return left.opcode < right.opcode;
    });
    for (const auto& [_, site] : sites) {
        report.unsupportedSites.push_back(site);
    }

    std::map<std::uint16_t, UnsupportedOpcodeSummary> summaries;
    for (const auto& site : report.unsupportedSites) {
        auto [iterator, inserted] = summaries.try_emplace(
            site.raw,
            UnsupportedOpcodeSummary{site.raw, 0, site.priority, {}});
        auto& summary = iterator->second;
        ++summary.count;
        summary.addresses.push_back(site.address);
        if (!inserted && priorityRank(site.priority) > priorityRank(summary.priority)) {
            summary.priority = site.priority;
        }
    }
    for (auto& [_, summary] : summaries) {
        report.unsupportedOpcodes.push_back(std::move(summary));
    }
    std::ranges::sort(report.unsupportedOpcodes, [](const auto& left, const auto& right) {
        const auto leftRank = priorityRank(left.priority);
        const auto rightRank = priorityRank(right.priority);
        if (leftRank != rightRank) {
            return leftRank > rightRank;
        }
        if (left.count != right.count) {
            return left.count > right.count;
        }
        return left.raw < right.raw;
    });
    return report;
}

std::string_view priorityName(AnalysisPriority priority) noexcept {
    switch (priority) {
    case AnalysisPriority::Critical: return "CRITICAL";
    case AnalysisPriority::High: return "HIGH";
    case AnalysisPriority::Medium: return "MEDIUM";
    }
    return "UNKNOWN";
}

std::string_view unsupportedContextName(UnsupportedContext context) noexcept {
    switch (context) {
    case UnsupportedContext::DelaySlot: return "delay-slot";
    case UnsupportedContext::BlockTerminator: return "block-stopper";
    case UnsupportedContext::Instruction: return "instruction";
    }
    return "unknown";
}

std::string_view delaySlotIssueName(DelaySlotIssueKind kind) noexcept {
    switch (kind) {
    case DelaySlotIssueKind::Missing: return "missing";
    case DelaySlotIssueKind::Unsupported: return "unsupported";
    case DelaySlotIssueKind::ControlTransfer: return "control-transfer";
    }
    return "unknown";
}

} // namespace saturn::sh2
