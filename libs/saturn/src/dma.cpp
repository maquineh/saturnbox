#include "saturn/saturn/dma.hpp"

#include "saturn/saturn/cd.hpp"
#include "saturn/sir/interpreter.hpp"

#include <algorithm>
#include <array>

namespace saturn::platform {
namespace {

[[nodiscard]] std::uint32_t widthMask(std::uint8_t width) noexcept {
    return width == 1U ? 0xFFU : width == 2U ? 0xFFFFU : 0xFFFFFFFFU;
}

[[nodiscard]] std::uint32_t readPart(std::uint32_t value, std::size_t byte,
                                     std::uint8_t width) noexcept {
    const auto shift = static_cast<unsigned>((4U - byte - width) * 8U);
    return (value >> shift) & widthMask(width);
}

[[nodiscard]] std::uint32_t mergePart(std::uint32_t original, std::size_t byte,
                                      std::uint8_t width, std::uint32_t value) noexcept {
    const auto shift = static_cast<unsigned>((4U - byte - width) * 8U);
    const auto mask = widthMask(width) << shift;
    return (original & ~mask) | ((value & widthMask(width)) << shift);
}

[[nodiscard]] bool isCdFifo(core::Address address) noexcept {
    return address == CdRuntime::kFifo || address == CdRuntime::kFifo + 0x20000000U;
}

[[nodiscard]] std::size_t writeIncrement(std::uint32_t add) noexcept {
    constexpr std::array<std::size_t, 8> increments{0U, 2U, 4U, 8U,
                                                     16U, 32U, 64U, 128U};
    return increments[add & 0x7U];
}

[[nodiscard]] std::size_t readIncrement(std::uint32_t add) noexcept {
    // DxRA is a one-bit selector: CS2 reads either hold the source address or
    // advance it by one SCU longword. FIFO reads are handled separately and
    // remain at the fixed CD data port address.
    return (add & 0x100U) != 0U ? 4U : 0U;
}

[[nodiscard]] bool readUpdates(std::uint32_t mode) noexcept {
    return (mode & 0x00010000U) != 0U;
}

[[nodiscard]] bool writeUpdates(std::uint32_t mode) noexcept {
    return (mode & 0x00000100U) != 0U;
}

[[nodiscard]] std::uint32_t startFactor(std::uint32_t mode) noexcept {
    return mode & 0x7U;
}

[[nodiscard]] std::size_t directTransferBytes(std::size_t channel,
                                               std::uint32_t count) noexcept {
    const auto mask = channel == 0U ? 0x000FFFFFU : 0x00000FFFU;
    const auto maximum = channel == 0U ? 0x00100000U : 0x00001000U;
    const auto masked = count & mask;
    return masked == 0U ? maximum : static_cast<std::size_t>(masked);
}

} // namespace

void ScuDmaRuntime::attach(sir::MachineState& state, CdRuntime& cd) noexcept {
    state_ = &state;
    cd_ = &cd;
}

void ScuDmaRuntime::setExecutionContext(std::size_t step, core::Address block,
                                        core::Address caller) noexcept {
    contextStep_ = step;
    contextBlock_ = block;
    contextCaller_ = caller;
}

std::optional<std::size_t> ScuDmaRuntime::channelFor(
    core::Address address, std::size_t& registerOffset) noexcept {
    if (address < kBase || address >= kBase + kChannelStride * kChannelCount) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(address - kBase);
    const auto channel = offset / kChannelStride;
    registerOffset = offset % kChannelStride;
    return channel;
}

std::optional<std::uint32_t> ScuDmaRuntime::read(core::Address address,
                                                 std::uint8_t width) const {
    if (width != 1U && width != 2U && width != 4U) return 0U;
    if ((address & (width - 1U)) != 0U) return 0U;

    if (address == kDmaStart || address == kDmaStatus) {
        const auto value = address == kDmaStart ? 0U :
            (status_[0] & 1U) | ((status_[1] & 1U) << 1U) | ((status_[2] & 1U) << 2U);
        return width == 4U ? value : readPart(value, address - (address & ~3U), width);
    }

    std::size_t offset{};
    const auto channel = channelFor(address, offset);
    if (!channel || offset > 0x14U || (offset != 0x10U && (offset & 0x3U) != 0U)) {
        return std::nullopt;
    }
    const auto& descriptor = channels_[*channel];
    const auto value = offset == 0x00U ? descriptor.source
        : offset == 0x04U ? descriptor.destination
        : offset == 0x08U ? descriptor.count
        : offset == 0x0CU ? descriptor.add
        : offset == 0x10U ? ((descriptor.enabled ? 0x100U : 0U) |
                             (descriptor.requested ? 1U : 0U))
        : descriptor.mode;
    return width == 4U ? value : readPart(value, address & 0x3U, width);
}

bool ScuDmaRuntime::write(core::Address address, std::uint8_t width,
                          std::uint32_t value) {
    if (width != 1U && width != 2U && width != 4U) return true;
    if ((address & (width - 1U)) != 0U) return true;
    value &= widthMask(width);

    if (address == kDmaStart) {
        const auto mask = width == 4U ? value : value << ((4U - width) * 8U);
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            if ((mask & (1U << channel)) != 0U) {
                channels_[channel].requested = true;
                start(channel);
            }
        }
        return true;
    }
    if (address == kDmaStatus) {
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            if ((value & (1U << channel)) != 0U) status_[channel] = 0U;
        }
        return true;
    }

    std::size_t offset{};
    const auto channel = channelFor(address, offset);
    if (!channel || offset > 0x14U || (offset != 0x10U && (offset & 0x3U) != 0U)) {
        return false;
    }
    auto& descriptor = channels_[*channel];
    auto* field = offset == 0x00U ? &descriptor.source
        : offset == 0x04U ? &descriptor.destination
        : offset == 0x08U ? &descriptor.count
        : offset == 0x0CU ? &descriptor.add
        : offset == 0x14U ? &descriptor.mode
        : nullptr;
    if (field != nullptr) {
        const auto current = *field;
        *field = width == 4U ? value : mergePart(current, address & 0x3U, width, value);
        if (offset == 0x14U && descriptor.enabled) {
            descriptor.requested = startFactor(descriptor.mode) !=
                static_cast<std::uint32_t>(ActivationFactor::Manual);
        }
        return true;
    }

    const auto current = (descriptor.enabled ? 0x100U : 0U) |
        (descriptor.requested ? 1U : 0U);
    const auto registerValue = width == 4U ? value
        : mergePart(current, address & 0x3U, width, value);
    const auto permission = (registerValue & 0x100U) != 0U;
    const auto wasEnabled = descriptor.enabled;
    const auto manualStart = startFactor(descriptor.mode) ==
        static_cast<std::uint32_t>(ActivationFactor::Manual);
    const auto bareGo = manualStart && (registerValue & 1U) != 0U &&
        !permission && !wasEnabled;
    descriptor.enabled = permission || bareGo;
    if (!descriptor.enabled) {
        descriptor.requested = false;
        status_[*channel] = 0U;
        return true;
    }
    if (manualStart) {
        if ((registerValue & 1U) != 0U) {
            descriptor.requested = true;
            start(*channel);
        }
    } else {
        // For factors 0..6, DxEN enables the channel and the selected device
        // edge starts each transfer; DxGO is the manual-start condition only.
        descriptor.requested = true;
    }
    return true;
}

void ScuDmaRuntime::onCdStateChanged() {
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        // A CD FIFO transfer only unblocks after the CD block publishes
        // DRDY. CSCT means a sector reached a partition, which can happen
        // before Get Sector Data has populated the host FIFO.
        if (channels_[channel].enabled && channels_[channel].requested &&
            startFactor(channels_[channel].mode) == 7U) {
            start(channel);
        }
    }
}

void ScuDmaRuntime::onVBlank() {
    onActivationFactor(ActivationFactor::VBlankIn);
}

void ScuDmaRuntime::onActivationFactor(ActivationFactor factor) {
    const auto activation = static_cast<std::uint32_t>(factor);
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        auto& descriptor = channels_[channel];
        if (descriptor.enabled && startFactor(descriptor.mode) == activation) {
            descriptor.requested = true;
            start(channel);
        }
    }
}

std::uint32_t ScuDmaRuntime::takeCompletionMask() noexcept {
    const auto mask = completionMask_;
    completionMask_ = 0U;
    return mask;
}

const std::array<std::uint32_t, ScuDmaRuntime::kChannelCount>&
ScuDmaRuntime::status() const noexcept {
    return status_;
}

std::array<ScuDmaRuntime::Descriptor, ScuDmaRuntime::kChannelCount>
ScuDmaRuntime::descriptors() const noexcept {
    std::array<Descriptor, kChannelCount> result{};
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        result[channel] = {channels_[channel].source, channels_[channel].destination,
                           channels_[channel].count, channels_[channel].add,
                           channels_[channel].mode, channels_[channel].enabled,
                           channels_[channel].requested};
    }
    return result;
}

const std::optional<ScuDmaRuntime::Transfer>& ScuDmaRuntime::lastTransfer() const noexcept {
    return lastTransfer_;
}

std::vector<ScuDmaRuntime::Transfer>
ScuDmaRuntime::transferHistory() const {
    return {transferHistory_.begin(), transferHistory_.end()};
}

std::vector<ScuDmaRuntime::Transfer>
ScuDmaRuntime::vdp1TextureTransferHistory() const {
    return {vdp1TextureTransferHistory_.begin(), vdp1TextureTransferHistory_.end()};
}

std::vector<ScuDmaRuntime::Transfer>
ScuDmaRuntime::spriteSourceBufferTransferHistory() const {
    return {spriteSourceBufferTransferHistory_.begin(),
            spriteSourceBufferTransferHistory_.end()};
}

void ScuDmaRuntime::start(std::size_t channel) {
    if (channel >= kChannelCount || state_ == nullptr || cd_ == nullptr) return;
    auto& descriptor = channels_[channel];
    if (!descriptor.enabled || !descriptor.requested) {
        status_[channel] = 0U;
        return;
    }

    const auto initialSource = descriptor.source;
    const auto initialDestination = descriptor.destination;
    const auto initialCount = descriptor.count;
    const auto indirect = (descriptor.mode & kIndirectMode) != 0U;
    status_[channel] = 1U;

    auto sourceAfter = initialSource;
    auto destinationAfter = initialDestination;
    std::size_t totalMoved{};
    bool complete = true;
    bool stoppedOnFifo{};
    bool indirectTableEnded = !indirect;
    auto tablePointer = initialDestination;
    std::size_t indirectEntries{};

    const auto executeTransfer = [&](core::Address sourceAddress,
                                     core::Address destinationAddress,
                                     std::size_t requestedBytes) {
        const auto fromCd = isCdFifo(sourceAddress);
        Transfer transfer{channel, sourceAddress, destinationAddress,
                          (requestedBytes + 3U) / 4U, 0U,
                          static_cast<std::uint32_t>(requestedBytes),
                          descriptor.add, descriptor.mode, 0U, 0U, 0U, 0U,
                          fromCd, false};
        auto source = sourceAddress;
        auto destination = destinationAddress;
        std::size_t moved{};
        std::size_t units{};
        while (moved < requestedBytes) {
            const auto sourceBefore = source;
            const auto destinationBefore = destination;
            std::size_t unitBytes{};
            if (!transferWord(channel, descriptor, source, destination,
                              requestedBytes - moved, unitBytes)) {
                break;
            }
            if (units == 0U) {
                transfer.firstSource = sourceBefore;
                transfer.firstDestination = destinationBefore;
            }
            transfer.lastSource = sourceBefore;
            transfer.lastDestination = destinationBefore;
            moved += unitBytes;
            ++units;
        }
        transfer.words = units;
        transfer.bytes = moved;
        transfer.complete = moved == requestedBytes;
        transfer.contextStep = contextStep_;
        transfer.contextBlock = contextBlock_;
        transfer.contextCaller = contextCaller_;
        lastTransfer_ = transfer;
        if (transferHistory_.size() == kTransferHistoryLimit) {
            transferHistory_.pop_front();
        }
        transferHistory_.push_back(transfer);

        // Keep texture uploads separately from high-frequency command-list
        // copies. SCU DMA can use the 0x25Cxxxxx cache-bypass alias, so
        // normalize to the physical 0x05Cxxxxx A-bus window first.
        const auto physicalDestination = transfer.destination & 0x1FFFFFFFU;
        const auto transferBegin = static_cast<std::uint64_t>(physicalDestination);
        const auto transferEnd = transferBegin + transfer.bytes;
        if (transferEnd > kVdp1TextureStart && transferBegin < kVdp1VramEnd) {
            if (vdp1TextureTransferHistory_.size() == kTransferHistoryLimit) {
                vdp1TextureTransferHistory_.pop_front();
            }
            vdp1TextureTransferHistory_.push_back(transfer);
        }
        if (transferEnd > kSpriteSourceBufferStart &&
            transferBegin < kSpriteSourceBufferEnd) {
            if (spriteSourceBufferTransferHistory_.size() == kTransferHistoryLimit) {
                spriteSourceBufferTransferHistory_.pop_front();
            }
            spriteSourceBufferTransferHistory_.push_back(transfer);
        }
        totalMoved += moved;
        sourceAfter = source;
        destinationAfter = destination;
        if (!transfer.complete) {
            complete = false;
            stoppedOnFifo = fromCd;
        }
        return transfer.complete;
    };

    if (!indirect) {
        const auto requestedBytes = directTransferBytes(channel, initialCount);
        static_cast<void>(executeTransfer(initialSource, initialDestination,
                                          requestedBytes));
        descriptor.count = static_cast<std::uint32_t>(requestedBytes - totalMoved);
        if (!writeUpdates(descriptor.mode)) destinationAfter = initialDestination;
    } else {
        // Sega specifies a 12-byte (count, write address, read address) table,
        // with bit 31 of the final read address marking the end. Bound malformed
        // guest lists while retaining the full 1 MiB indirect transfer limit.
        constexpr std::size_t kMaxIndirectEntries = 65536U;
        const auto initialTablePointer = initialDestination;
        std::size_t indirectBytes{};
        complete = false;
        for (; indirectEntries < kMaxIndirectEntries;
             ++indirectEntries, tablePointer += 12U) {
            const auto countWord = state_->read32BE(tablePointer);
            const auto destination = state_->read32BE(
                static_cast<core::Address>(tablePointer + 4U));
            const auto taggedSource = state_->read32BE(
                static_cast<core::Address>(tablePointer + 8U));
            const auto isLast = (taggedSource & 0x80000000U) != 0U;
            const auto source = taggedSource & 0x7FFFFFFFU;
            const auto encodedCount = countWord & 0x000FFFFFU;
            const auto requestedBytes = encodedCount == 0U
                ? kIndirectTransferLimit : static_cast<std::size_t>(encodedCount);
            if (requestedBytes > kIndirectTransferLimit - indirectBytes ||
                isCdFifo(source)) {
                // Sega prohibits indirect SCU DMA from the CD block FIFO.
                break;
            }
            if (!executeTransfer(source, destination, requestedBytes)) break;
            indirectBytes += requestedBytes;
            if (isLast) {
                tablePointer += 12U;
                indirectTableEnded = true;
                complete = true;
                break;
            }
        }
        destinationAfter = writeUpdates(descriptor.mode)
            ? tablePointer : initialTablePointer;
        descriptor.count = initialCount; // DxC is not used in indirect mode.
        if (!indirectTableEnded) complete = false;
    }

    // In indirect mode DxW is the descriptor-list cursor. DxRUP/DxWUP decide
    // which final address values are committed to the visible registers.
    descriptor.source = readUpdates(descriptor.mode) ? sourceAfter : initialSource;
    descriptor.destination = destinationAfter;
    if (!indirect && !complete) {
        descriptor.count = static_cast<std::uint32_t>(
            directTransferBytes(channel, initialCount) - totalMoved);
    }
    if (complete) {
        completionMask_ |= static_cast<std::uint32_t>(1U << channel);
        descriptor.requested = false;
        status_[channel] = 0U;
    } else if (stoppedOnFifo) {
        // Direct CD FIFO transfers resume when the CD runtime publishes data.
        status_[channel] = 1U;
    } else {
        descriptor.requested = false;
        status_[channel] = 0U;
    }
}

bool ScuDmaRuntime::transferWord(std::size_t channel, Channel& descriptor,
                                 core::Address& source, core::Address& destination,
                                 std::size_t bytesRemaining, std::size_t& bytesMoved) {
    static_cast<void>(channel);
    bytesMoved = 0U;
    if (state_ == nullptr || cd_ == nullptr) return false;
    if (isCdFifo(source) && !cd_->fifoDataAvailable()) return false;

    const auto transferBytes = std::min<std::size_t>(4U, bytesRemaining);
    std::uint32_t value = 0U;
    if (transferBytes == 4U) {
        value = state_->read32BE(source);
    } else {
        for (std::size_t index = 0; index < transferBytes; ++index) {
            value = (value << 8U) | state_->read8(
                static_cast<core::Address>(source + index));
        }
    }

    const auto writeAdd = writeIncrement(descriptor.add);
    if (transferBytes == 4U) {
        if (writeAdd == 2U) {
            state_->write16BE(destination, static_cast<std::uint16_t>(value >> 16U));
            state_->write16BE(static_cast<core::Address>(destination + 2U),
                              static_cast<std::uint16_t>(value));
        } else {
            state_->write32BE(destination, value);
        }
    } else if (transferBytes == 2U) {
        state_->write16BE(destination, static_cast<std::uint16_t>(value));
    } else {
        state_->write8(destination, static_cast<std::uint8_t>(value));
    }
    if (!isCdFifo(source)) {
        source = static_cast<core::Address>(source + readIncrement(descriptor.add));
    }
    const auto increment = writeAdd == 2U && transferBytes == 4U ? 4U : writeAdd;
    destination = static_cast<core::Address>(destination + increment);
    bytesMoved = transferBytes;
    return true;
}

} // namespace saturn::platform
