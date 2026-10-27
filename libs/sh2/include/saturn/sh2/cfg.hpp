#pragma once

#include "saturn/core/memory_map.hpp"
#include "saturn/sh2/decoder.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace saturn::sh2 {

enum class EdgeKind : std::uint8_t {
    Fallthrough,
    Taken,
    NotTaken,
    Call,
    ReturnSite,
    Indirect,
};

struct CFGEdge {
    std::optional<Address> target;
    EdgeKind kind{EdgeKind::Fallthrough};
};

struct DelayedTerminator {
    DecodedInstruction transfer;
    std::optional<DecodedInstruction> slot;
};

struct BasicBlock {
    Address start{};
    std::vector<DecodedInstruction> instructions;
    std::optional<DelayedTerminator> terminator;
    std::vector<CFGEdge> successors;
};

enum class FunctionEvidence : std::uint8_t {
    EntryPoint,
    DirectCall,
};

struct FunctionCandidate {
    Address address{};
    Confidence confidence{Confidence::Unknown};
    FunctionEvidence evidence{FunctionEvidence::EntryPoint};
};

struct CFG {
    std::map<Address, BasicBlock> blocks;
    std::vector<FunctionCandidate> functionCandidates;

    [[nodiscard]] const BasicBlock* blockAt(Address address) const noexcept;
};

// Discovers reachable instructions first, then splits them at every leader.
// This keeps blocks valid even when a backward branch reaches the middle of a
// previously linear decode sequence.
class BlockBuilder {
public:
    explicit BlockBuilder(Decoder decoder = {});

    [[nodiscard]] CFG build(const core::MemoryMap& image,
                            std::span<const Address> entries) const;

private:
    Decoder decoder_;
};

[[nodiscard]] std::string toDot(const CFG& cfg);

} // namespace saturn::sh2
