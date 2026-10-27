#pragma once

#include "saturn/sh2/cfg.hpp"
#include "saturn/sh2/module_lifter.hpp"
#include "saturn/sir/interpreter.hpp"

#include <optional>

namespace saturn::sh2 {

struct ControlObservation {
    sir::ControlKind kind{sir::ControlKind::None};
    std::optional<Address> target;
    std::optional<Address> returnSite;
    std::uint8_t trapVector{};
    bool slept{};

    [[nodiscard]] bool operator==(const ControlObservation& other) const noexcept = default;
};

enum class ReferenceStatus : std::uint8_t {
    Success,
    InvalidBlock,
    UnsupportedInstruction,
};

struct ReferenceResult {
    ReferenceStatus status{ReferenceStatus::Success};
    Address address{};
    ControlObservation control;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == ReferenceStatus::Success;
    }
};

class ReferenceExecutor {
public:
    [[nodiscard]] ReferenceResult execute(const BasicBlock& block,
                                          sir::MachineState& state) const;
};

struct DifferentialResult {
    ReferenceResult reference;
    sir::ExecutionResult interpreted;
    ControlObservation sirControl;
    bool stateMatches{};
    bool controlMatches{};

    [[nodiscard]] bool matches() const noexcept {
        return reference && interpreted && stateMatches && controlMatches;
    }
};

[[nodiscard]] DifferentialResult compareBlock(const BasicBlock& source,
                                              const sir::BasicBlock& lowered,
                                              const sir::MachineState& initial);

} // namespace saturn::sh2
