#pragma once

#include <cstdint>
#include <functional>

namespace saturn::platform {

struct M68kBusBridge;

// Bus-facing wrapper around the Musashi 68000 core used for the Saturn SCSP
// sound processor. A 68EC000 has the 68000 instruction set without an MMU;
// Musashi's 68000 mode is the closest supported CPU model.
class M68k final {
public:
    struct Bus {
        std::function<std::uint8_t(std::uint32_t)> read8;
        std::function<std::uint16_t(std::uint32_t)> read16;
        std::function<std::uint32_t(std::uint32_t)> read32;
        std::function<void(std::uint32_t, std::uint8_t)> write8;
        std::function<void(std::uint32_t, std::uint16_t)> write16;
        std::function<void(std::uint32_t, std::uint32_t)> write32;
    };

    struct Stats {
        std::uint64_t executeCalls{};
        std::uint64_t cyclesExecuted{};
        std::uint32_t lastCycleCount{};
    };

    explicit M68k(Bus bus);
    ~M68k();

    M68k(const M68k&) = delete;
    M68k& operator=(const M68k&) = delete;
    M68k(M68k&&) = delete;
    M68k& operator=(M68k&&) = delete;

    // Reads the initial SSP and PC from bus addresses 0 and 4.
    void reset();
    // Musashi may finish the current instruction after the requested budget;
    // the return value and stats report the cycles actually consumed.
    [[nodiscard]] std::uint32_t execute(std::uint32_t cycleBudget);
    // Sets the asserted 68000 IRQ level (0 clears IRQ; 1..7 assert).
    void setIrq(std::uint8_t level);

    [[nodiscard]] std::uint32_t pc() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept;

private:
    friend struct M68kBusBridge;
    Bus bus_;
    Stats stats_{};
    std::uint8_t irqLevel_{};
    std::uint32_t resetCyclesPending_{};
    bool reset_{};
};

} // namespace saturn::platform
