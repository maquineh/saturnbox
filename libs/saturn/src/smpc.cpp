#include "saturn/saturn/smpc.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace saturn::platform {

void DigitalPadState::set(PadButton button, bool down) noexcept {
    const auto mask = static_cast<std::uint16_t>(button);
    pressed = down ? static_cast<std::uint16_t>(pressed | mask)
                   : static_cast<std::uint16_t>(pressed & ~mask);
}

bool DigitalPadState::isPressed(PadButton button) const noexcept {
    return (pressed & static_cast<std::uint16_t>(button)) != 0;
}

std::optional<std::uint32_t> SmpcRuntime::read(core::Address address,
                                               std::uint8_t width) const {
    if (width != 1) {
        return 0U;
    }
    if (address >= kOreg0 && address <= kOreg31 && ((address - kOreg0) & 1U) == 0U) {
        return oregs_[(address - kOreg0) / 2U];
    }
    if (address == kStatus) {
        return status_;
    }
    if (address == kStatusFlag) {
        return statusFlag_ & 1U;
    }
    return 0U;
}

bool SmpcRuntime::write(core::Address address, std::uint8_t width,
                        std::uint32_t value) {
    if (width != 1) {
        return true;
    }
    const auto byte = static_cast<std::uint8_t>(value);
    if (address >= kIreg0 && address <= kIreg6 && ((address - kIreg0) & 1U) == 0U) {
        const auto previous = iregs_[0];
        iregs_[(address - kIreg0) / 2U] = byte;
        if (address == kIreg0 && (byte & 0x40U) != 0U) {
            intbackPending_ = false; // BREAK ends this peripheral exchange.
        } else if (address == kIreg0 && intbackPending_ && intbackStatusPhase_ &&
                   ((previous ^ byte) & 0x80U) != 0U) {
            // Status+peripheral requests require an explicit CONTINUE
            // between the two packets; do not expose pad bytes as RTC data.
            intbackStatusPhase_ = false;
            executeIntback();
        }
        return true;
    }
    if (address == kStatusFlag) {
        statusFlag_ = byte & 1U;
        return true;
    }
    if (address == kComreg) {
        command_ = byte;
        executeCommand(byte);
        return true;
    }
    return true;
}

void SmpcRuntime::setPad(std::size_t port, DigitalPadState state) noexcept {
    if (port < pads_.size()) {
        pads_[port] = state;
    }
}

void SmpcRuntime::onVBlank() {
    if (intbackPending_ && !intbackStatusPhase_) executeIntback();
}

bool SmpcRuntime::takeInterruptRequest() noexcept {
    const auto requested = interruptRequested_;
    interruptRequested_ = false;
    return requested;
}

const DigitalPadState& SmpcRuntime::pad(std::size_t port) const noexcept {
    return pads_[std::min(port, pads_.size() - 1U)];
}

std::uint64_t SmpcRuntime::padSampleCount(std::size_t port) const noexcept {
    return padSampleCounts_[std::min(port, padSampleCounts_.size() - 1U)];
}

void SmpcRuntime::setResetPressed(bool pressed) noexcept { resetPressed_ = pressed; }
bool SmpcRuntime::resetPressed() const noexcept { return resetPressed_; }
bool SmpcRuntime::resetRequested() const noexcept { return resetRequested_; }
void SmpcRuntime::clearResetRequested() noexcept { resetRequested_ = false; }

std::array<std::uint8_t, 2> SmpcRuntime::encodePad(
    const DigitalPadState& state) noexcept {
    std::array<std::uint8_t, 2> data{0xFFU, 0xFFU};
    constexpr std::array firstButtons{
        PadButton::Right, PadButton::Left, PadButton::Down, PadButton::Up,
        PadButton::Start, PadButton::A, PadButton::C, PadButton::B};
    constexpr std::array secondButtons{
        PadButton::R, PadButton::X, PadButton::Y, PadButton::Z, PadButton::L};
    for (std::size_t bit = 0; bit < firstButtons.size(); ++bit) {
        if (state.isPressed(firstButtons[bit])) {
            data[0] &= static_cast<std::uint8_t>(~(0x80U >> bit));
        }
    }
    for (std::size_t bit = 0; bit < secondButtons.size(); ++bit) {
        if (state.isPressed(secondButtons[bit])) {
            data[1] &= static_cast<std::uint8_t>(~(0x80U >> bit));
        }
    }
    return data;
}

void SmpcRuntime::appendPort(std::size_t port, std::size_t& output) {
    if (output >= oregs_.size() - 1U) {
        return;
    }
    const auto& state = pads_[port];
    oregs_[output++] = state.connected ? 0xF1U : 0xF0U;
    if (!state.connected || output >= oregs_.size() - 3U) {
        return;
    }
    oregs_[output++] = 0x02U; // digital device, two data bytes
    const auto data = encodePad(state);
    ++padSampleCounts_[port];
    oregs_[output++] = data[0];
    oregs_[output++] = data[1];
}

void SmpcRuntime::executeIntback() {
    oregs_.fill(0);
    const auto portModes = static_cast<std::uint8_t>(iregs_[1] >> 4U);
    if (intbackStatusPhase_) {
        // This HLE subset leaves RTC/SMEM fields zero. SR bit 7 distinguishes
        // status from peripheral data; bit 6 announces the following packet.
        status_ = (iregs_[1] & 0x08U) != 0U ? 0x40U : 0U;
        oregs_[0] = resetEnabled_ ? 0U : 0x80U;
        oregs_[31] = kIntback;
        interruptRequested_ = true;
        return;
    }
    // Both digital pads fit in the first packet: PDL=1, NPE=0.
    // Port modes occupy SR bits 3:0, not the packet-location bits.
    status_ = static_cast<std::uint8_t>(0xC0U | portModes |
                                       (resetPressed_ ? 0x10U : 0U));
    std::size_t output = 0;
    if ((iregs_[1] & 0x08U) != 0U) {
        if ((iregs_[1] & 0x30U) != 0x30U) {
            appendPort(0, output);
        }
        if ((iregs_[1] & 0xC0U) != 0xC0U) {
            appendPort(1, output);
        }
    }
    oregs_[31] = kIntback;
    interruptRequested_ = true;
}

void SmpcRuntime::executeCommand(std::uint8_t command) {
    statusFlag_ = 1U;
    switch (command) {
    case 0x02U:
        slaveEnabled_ = true;
        oregs_[31] = command;
        break;
    case 0x03U:
        slaveEnabled_ = false;
        ++slaveResetGeneration_;
        oregs_[31] = command;
        break;
    case kIntback:
        intbackPending_ = true;
        intbackStatusPhase_ = (iregs_[0] & 1U) != 0U;
        executeIntback();
        break;
    case 0x06U:
        soundCpuEnabled_ = true;
        oregs_[31] = command;
        break;
    case 0x07U:
        soundCpuEnabled_ = false;
        oregs_[31] = command;
        break;
    case 0x0DU: resetRequested_ = resetEnabled_; break;
    case 0x19U: resetEnabled_ = true; break;
    case 0x1AU: resetEnabled_ = false; break;
    default:
        oregs_[31] = command;
        break;
    }
    statusFlag_ = 0U;
}

std::optional<PadButton> parsePadButton(std::string_view name) noexcept {
    std::string normalized(name);
    std::ranges::transform(normalized, normalized.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (normalized == "right") return PadButton::Right;
    if (normalized == "left") return PadButton::Left;
    if (normalized == "down") return PadButton::Down;
    if (normalized == "up") return PadButton::Up;
    if (normalized == "start") return PadButton::Start;
    if (normalized == "a") return PadButton::A;
    if (normalized == "b") return PadButton::B;
    if (normalized == "c") return PadButton::C;
    if (normalized == "x") return PadButton::X;
    if (normalized == "y") return PadButton::Y;
    if (normalized == "z") return PadButton::Z;
    if (normalized == "l") return PadButton::L;
    if (normalized == "r") return PadButton::R;
    return std::nullopt;
}

} // namespace saturn::platform
