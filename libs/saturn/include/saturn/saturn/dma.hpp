#pragma once

#include "saturn/core/address.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace saturn::sir {
class MachineState;
}

namespace saturn::platform {

class CdRuntime;

// Deterministic SCU A-bus DMA subset. The Saturn exposes three channels with
// source, destination, byte-count, address-add, permission/GO and mode
// registers. This runtime performs direct transfers and keeps a CD FIFO
// transfer pending until the CD Block has produced a sector (CSCT).
class ScuDmaRuntime final {
public:
    static constexpr core::Address kBase = 0x05FE0000U;
    static constexpr core::Address kDma0 = kBase + 0x000U;
    static constexpr core::Address kDma1 = kBase + 0x020U;
    static constexpr core::Address kDma2 = kBase + 0x040U;
    static constexpr core::Address kDmaStart = kBase + 0x098U;
    static constexpr core::Address kDmaStatus = kBase + 0x09CU;
    static constexpr std::size_t kChannelCount = 3U;
    static constexpr std::size_t kChannelStride = 0x20U;
    static constexpr std::size_t kTransferHistoryLimit = 8192U;
    static constexpr core::Address kVdp1TextureStart = 0x05C60000U;
    static constexpr core::Address kVdp1VramEnd = 0x05C80000U;
    static constexpr core::Address kSpriteSourceBufferStart = 0x06003000U;
    static constexpr core::Address kSpriteSourceBufferEnd = 0x06003780U;
    static constexpr std::uint32_t kIndirectMode = 0x01000000U;
    static constexpr std::size_t kIndirectTransferLimit = 0x00100000U;

    enum class ActivationFactor : std::uint8_t {
        VBlankIn = 0U,
        VBlankOut = 1U,
        HBlankIn = 2U,
        Timer0 = 3U,
        Timer1 = 4U,
        ScspRequest = 5U,
        SpriteDrawEnd = 6U,
        Manual = 7U,
    };

    struct Transfer {
        std::size_t channel{};
        core::Address source{};
        core::Address destination{};
        std::size_t words{};
        std::size_t bytes{};
        std::uint32_t count{};
        std::uint32_t add{};
        std::uint32_t mode{};
        core::Address firstSource{};
        core::Address firstDestination{};
        core::Address lastSource{};
        core::Address lastDestination{};
        bool fromCd{};
        bool complete{};
        // SH-2 execution context active when the synchronous DMA transfer
        // emitted its MachineState writes. These fields are diagnostic only.
        std::size_t contextStep{};
        core::Address contextBlock{};
        core::Address contextCaller{};
    };

    struct Descriptor {
        std::uint32_t source{};
        std::uint32_t destination{};
        std::uint32_t count{};
        std::uint32_t add{};
        std::uint32_t mode{7U};
        bool enabled{};
        bool requested{};
    };

    void attach(sir::MachineState& state, CdRuntime& cd) noexcept;
    void setExecutionContext(std::size_t step, core::Address block,
                             core::Address caller) noexcept;

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width) const;
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                             std::uint32_t value);

    // Called by MmioInterceptor after a CD command changes FIFO state.
    void onCdStateChanged();
    // Called at the VBlank-IN edge for channels selecting that activation
    // factor (DxFT=000).  Manual channels (DxFT=111) still start on DxGO.
    void onVBlank();
    // Called when a modeled SCU activation edge occurs. Automatic factors are
    // re-armed by each matching edge while DxEN remains enabled.
    void onActivationFactor(ActivationFactor factor);

    // Returns channels whose transfer reached its terminal count since the
    // previous call.  The SCU exposes these completions as interrupt factors;
    // keeping the edge pending here lets the host scheduler deliver the
    // corresponding vector at the next SH-2 block boundary.
    [[nodiscard]] std::uint32_t takeCompletionMask() noexcept;

    [[nodiscard]] const std::array<std::uint32_t, kChannelCount>& status() const noexcept;
    [[nodiscard]] std::array<Descriptor, kChannelCount> descriptors() const noexcept;
    [[nodiscard]] const std::optional<Transfer>& lastTransfer() const noexcept;
    [[nodiscard]] std::vector<Transfer> transferHistory() const;
    [[nodiscard]] std::vector<Transfer> vdp1TextureTransferHistory() const;
    [[nodiscard]] std::vector<Transfer> spriteSourceBufferTransferHistory() const;

private:
    struct Channel {
        std::uint32_t source{};
        std::uint32_t destination{};
        std::uint32_t count{};
        std::uint32_t add{};
        std::uint32_t mode{7U};
        bool enabled{};
        bool requested{};
    };

    [[nodiscard]] static std::optional<std::size_t> channelFor(
        core::Address address, std::size_t& registerOffset) noexcept;
    void start(std::size_t channel);
    [[nodiscard]] bool transferWord(std::size_t channel, Channel& descriptor,
                                    core::Address& source, core::Address& destination,
                                    std::size_t bytesRemaining, std::size_t& bytesMoved);

    std::array<Channel, kChannelCount> channels_{};
    std::array<std::uint32_t, kChannelCount> status_{};
    std::uint32_t completionMask_{};
    std::optional<Transfer> lastTransfer_;
    std::deque<Transfer> transferHistory_;
    std::deque<Transfer> vdp1TextureTransferHistory_;
    std::deque<Transfer> spriteSourceBufferTransferHistory_;
    sir::MachineState* state_{};
    CdRuntime* cd_{};
    std::size_t contextStep_{};
    core::Address contextBlock_{};
    core::Address contextCaller_{};
};

} // namespace saturn::platform
