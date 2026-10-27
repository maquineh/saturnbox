#pragma once

#include "saturn/saturn/smpc.hpp"
#include <windows.h>
#include <Xinput.h>
#include <array>
#include <bitset>
#include <chrono>
#include <utility>

namespace saturn::host {

class Win32Input {
public:
    void attach(platform::SmpcRuntime& smpc, std::array<bool, 2> scriptedPorts) {
        smpc_ = &smpc;
        scriptedPorts_ = scriptedPorts;
        poll();
    }
    void setScriptedPort(std::size_t port, bool scripted) {
        if (port >= scriptedPorts_.size()) return;
        scriptedPorts_[port] = scripted;
        if (!scripted) poll();
    }
    void setKey(std::size_t key, bool down) noexcept {
        if (key >= keyboard_.size()) return;
        if (down && !keyboard_[key]) {
            releaseFrame_[key] = frame_ + 2U;
            pressSample_[key] = smpc_ ? smpc_->padSampleCount(0U) : 0U;
        }
        keyboard_.set(key, down);
    }
    void advanceFrame() noexcept {
        ++frame_;
    }
    void setFocus(bool focused) {
        focused_ = focused;
        if (!focused) {
            keyboard_.reset();
            releaseFrame_.fill(0U);
        }
        poll();
    }
    void poll() {
        if (!smpc_) return;
        using Button = platform::PadButton;
        const auto now = Clock::now();
        for (std::size_t port = 0U; port < controllers_.size(); ++port) {
            if (scriptedPorts_[port]) continue;
            if (now >= nextPoll_[port]) {
                XINPUT_STATE state{};
                connected_[port] = XInputGetState(static_cast<DWORD>(port), &state) == ERROR_SUCCESS;
                controllers_[port] = state.Gamepad;
                // Avoid polling absent devices at every message/frame boundary.
                nextPoll_[port] = now + std::chrono::milliseconds(connected_[port] ? 8 : 1000);
            }
            platform::DigitalPadState pad{port == 0U || connected_[port], 0U};
            if (focused_) {
                if (port == 0U) {
                    constexpr std::array keys{
                        std::pair{VK_RIGHT, Button::Right}, std::pair{VK_LEFT, Button::Left},
                        std::pair{VK_DOWN, Button::Down}, std::pair{VK_UP, Button::Up},
                        std::pair{VK_RETURN, Button::Start},
                        std::pair{int('Z'), Button::A}, std::pair{int('X'), Button::B},
                        std::pair{int('C'), Button::C}, std::pair{int('A'), Button::X},
                        std::pair{int('S'), Button::Y}, std::pair{int('D'), Button::Z},
                        std::pair{int('Q'), Button::L}, std::pair{int('E'), Button::R}};
                    // A complete tap can arrive in one message pump. Keep it
                    // across a guest VBlank and a peripheral packet instead
                    // of losing both edges during the SMPC status phase.
                    for (const auto& [key, button] : keys)
                        pad.set(button, keyboard_[key] || frame_ < releaseFrame_[key] ||
                            (releaseFrame_[key] != 0U && smpc_->padSampleCount(0U) <= pressSample_[key]));
                }
                if (connected_[port]) {
                    const auto& state = controllers_[port];
                    constexpr std::array buttons{
                        std::pair{XINPUT_GAMEPAD_DPAD_RIGHT, Button::Right},
                        std::pair{XINPUT_GAMEPAD_DPAD_LEFT, Button::Left},
                        std::pair{XINPUT_GAMEPAD_DPAD_DOWN, Button::Down},
                        std::pair{XINPUT_GAMEPAD_DPAD_UP, Button::Up},
                        std::pair{XINPUT_GAMEPAD_START, Button::Start},
                        std::pair{XINPUT_GAMEPAD_X, Button::A}, std::pair{XINPUT_GAMEPAD_A, Button::B},
                        std::pair{XINPUT_GAMEPAD_B, Button::C}, std::pair{XINPUT_GAMEPAD_Y, Button::Y},
                        std::pair{XINPUT_GAMEPAD_LEFT_SHOULDER, Button::X},
                        std::pair{XINPUT_GAMEPAD_RIGHT_SHOULDER, Button::Z}};
                    for (const auto& [mask, button] : buttons) {
                        if ((state.wButtons & mask) != 0U) pad.set(button);
                    }
                    if (state.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) pad.set(Button::Right);
                    if (state.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) pad.set(Button::Left);
                    if (state.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) pad.set(Button::Up);
                    if (state.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) pad.set(Button::Down);
                    if (state.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) pad.set(Button::L);
                    if (state.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) pad.set(Button::R);
                }
                if (pad.isPressed(Button::Left) && pad.isPressed(Button::Right)) {
                    pad.set(Button::Left, false); pad.set(Button::Right, false);
                }
                if (pad.isPressed(Button::Up) && pad.isPressed(Button::Down)) {
                    pad.set(Button::Up, false); pad.set(Button::Down, false);
                }
            }
            const auto& previous = smpc_->pad(port);
            if (pad.connected != previous.connected || pad.pressed != previous.pressed) smpc_->setPad(port, pad);
        }
    }
private:
    using Clock = std::chrono::steady_clock;
    platform::SmpcRuntime* smpc_{};
    std::bitset<256U> keyboard_;
    std::array<std::uint64_t, 256U> releaseFrame_{};
    std::array<std::uint64_t, 256U> pressSample_{};
    std::uint64_t frame_{};
    std::array<bool, 2> scriptedPorts_{}, connected_{};
    std::array<XINPUT_GAMEPAD, 2> controllers_{};
    std::array<Clock::time_point, 2> nextPoll_{};
    bool focused_{};
};
} // namespace saturn::host
