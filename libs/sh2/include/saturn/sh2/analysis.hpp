#pragma once

#include "saturn/sh2/cfg.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace saturn::sh2 {

enum class AnalysisPriority : std::uint8_t {
    Critical,
    High,
    Medium,
};

enum class UnsupportedContext : std::uint8_t {
    DelaySlot,
    BlockTerminator,
    Instruction,
};

struct UnsupportedSite {
    Address address{};
    Address block{};
    std::uint16_t raw{};
    AnalysisPriority priority{AnalysisPriority::Medium};
    UnsupportedContext context{UnsupportedContext::Instruction};
};

struct UnsupportedOpcodeSummary {
    std::uint16_t raw{};
    std::size_t count{};
    AnalysisPriority priority{AnalysisPriority::Medium};
    std::vector<Address> addresses;
};

struct UnresolvedIndirectTransfer {
    Address address{};
    Address block{};
    FlowKind flow{FlowKind::IndirectBranch};
};

struct UnliftedOpcodeSummary {
    Opcode opcode{Opcode::Invalid};
    std::size_t count{};
    std::vector<Address> addresses;
};

enum class DelaySlotIssueKind : std::uint8_t {
    Missing,
    Unsupported,
    ControlTransfer,
};

struct DelaySlotIssue {
    Address transferAddress{};
    std::optional<Address> slotAddress;
    Address block{};
    DelaySlotIssueKind kind{DelaySlotIssueKind::Missing};
};

struct AnalysisReport {
    std::size_t basicBlockCount{};
    std::size_t instructionCount{};
    std::size_t fallthroughInstructionCount{};
    std::size_t liftableInstructionCount{};
    std::size_t interruptedBlockCount{};
    std::size_t invalidDelaySlotCount{};
    std::size_t missingDelaySlotCount{};
    std::size_t controlTransferDelaySlotCount{};
    std::vector<UnsupportedSite> unsupportedSites;
    std::vector<UnsupportedOpcodeSummary> unsupportedOpcodes;
    std::vector<UnliftedOpcodeSummary> unliftedOpcodes;
    std::vector<UnresolvedIndirectTransfer> unresolvedIndirectTransfers;
    std::vector<DelaySlotIssue> delaySlotIssues;
};

[[nodiscard]] AnalysisReport analyzeCFG(const CFG& cfg);
[[nodiscard]] std::string_view priorityName(AnalysisPriority priority) noexcept;
[[nodiscard]] std::string_view unsupportedContextName(UnsupportedContext context) noexcept;
[[nodiscard]] std::string_view delaySlotIssueName(DelaySlotIssueKind kind) noexcept;

} // namespace saturn::sh2
