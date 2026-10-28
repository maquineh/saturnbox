#pragma once

#include "saturn/core/address.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace saturn::platform {

enum class PadButton : std::uint16_t {
    Right = 1U << 0U,
    Left = 1U << 1U,
    Down = 1U << 2U,
    Up = 1U << 3U,
    Start = 1U << 4U,
    A = 1U << 5U,
    C = 1U << 6U,
    B = 1U << 7U,
    R = 1U << 8U,
    X = 1U << 9U,
    Y = 1U << 10U,
    Z = 1U << 11U,
    L = 1U << 12U,
};

struct DigitalPadState {
    bool connected{true};
    std::uint16_t pressed{};

    void set(PadButton button, bool down = true) noexcept;
    [[nodiscard]] bool isPressed(PadButton button) const noexcept;
};

// Synchronous, deterministic subset of the Saturn SMPC. It models the byte
// register interface and enough INTBACK behavior to expose two digital pads.
class SmpcRuntime {
public:
    static constexpr core::Address kIreg0 = 0x20100001U;
    static constexpr core::Address kIreg1 = 0x20100003U;
    static constexpr core::Address kIreg2 = 0x20100005U;
    static constexpr core::Address kIreg6 = 0x2010000DU;
    static constexpr core::Address kComreg = 0x2010001FU;
    static constexpr core::Address kOreg0 = 0x20100021U;
    static constexpr core::Address kOreg31 = 0x2010005FU;
    static constexpr core::Address kStatus = 0x20100061U;
    static constexpr core::Address kStatusFlag = 0x20100063U;
    static constexpr std::uint8_t kIntback = 0x10U;

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) const;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                             std::uint32_t value);

    void setPad(std::size_t port, DigitalPadState state) noexcept;
    [[nodiscard]] const DigitalPadState& pad(std::size_t port) const noexcept;
    // Counts peripheral packets that actually encoded this port, excluding
    // the status/RTC phase. Live input uses it to retain very short taps.
    [[nodiscard]] std::uint64_t padSampleCount(std::size_t port) const noexcept;
    void setResetPressed(bool pressed) noexcept;
    // Refresh an armed INTBACK exchange at the VBlank boundary so pad state
    // is sampled in the same phase as the Saturn's SMPC scanout.
    void onVBlank();
    [[nodiscard]] bool takeInterruptRequest() noexcept;
    [[nodiscard]] bool resetPressed() const noexcept;
    [[nodiscard]] bool resetRequested() const noexcept;
    void clearResetRequested() noexcept;
    [[nodiscard]] bool slaveEnabled() const noexcept { return slaveEnabled_; }
    [[nodiscard]] std::uint64_t slaveResetGeneration() const noexcept { return slaveResetGeneration_; }
    [[nodiscard]] bool soundCpuEnabled() const noexcept { return soundCpuEnabled_; }

private:
    void executeCommand(std::uint8_t command);
    void executeIntback();
    void appendPort(std::size_t port, std::size_t& output);
    [[nodiscard]] static std::array<std::uint8_t, 2> encodePad(
        const DigitalPadState& state) noexcept;

    std::array<std::uint8_t, 7> iregs_{};
    std::array<std::uint8_t, 32> oregs_{};
    std::array<DigitalPadState, 2> pads_{{DigitalPadState{}, DigitalPadState{false, 0}}};
    std::array<std::uint64_t, 2> padSampleCounts_{};
    std::uint8_t command_{};
    std::uint8_t status_{};
    std::uint8_t statusFlag_{};
    bool resetPressed_{};
    bool resetEnabled_{true};
    bool resetRequested_{};
    bool intbackPending_{};
    bool intbackStatusPhase_{};
    bool interruptRequested_{};
    bool slaveEnabled_{};
    bool soundCpuEnabled_{};
    std::uint64_t slaveResetGeneration_{};
};

[[nodiscard]] std::optional<PadButton> parsePadButton(std::string_view name) noexcept;

} // namespace saturn::platform
