#pragma once

#include "saturn/core/address.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace saturn::sir {
class MachineState;
}

namespace saturn::platform {

// Deterministic HLE for the Saturn CD block command/FIFO interface. The
// runtime deliberately models the data path needed by SH-2 software without
// emulating the CD servo or its asynchronous timing.
class CdRuntime {
public:
    static constexpr std::size_t kLogicalSectorSize = 2048U;
    static constexpr core::Address kBase = 0x05800000U;
    static constexpr core::Address kCommandBase = kBase + 0x00090000U;
    static constexpr core::Address kFifo = kBase + 0x00018000U;
    // The A-bus exposes a separate alias for 16-bit CPU transfers. Sega's
    // CDC library uses this address for TOC and file-info payloads.
    static constexpr core::Address kWordFifo = kBase + 0x00098000U;

    static constexpr std::uint16_t kHirqCmok = 0x0001U;
    static constexpr std::uint16_t kHirqDrdy = 0x0002U;
    static constexpr std::uint16_t kHirqCsct = 0x0004U;
    static constexpr std::uint16_t kHirqBful = 0x0008U;
    static constexpr std::uint16_t kHirqPend = 0x0010U;
    static constexpr std::uint16_t kHirqDchg = 0x0020U;
    static constexpr std::uint16_t kHirqEsel = 0x0040U;
    static constexpr std::uint16_t kHirqEhst = 0x0080U;
    static constexpr std::uint16_t kHirqEcpy = 0x0100U;
    static constexpr std::uint16_t kHirqEfls = 0x0200U;
    static constexpr std::uint16_t kHirqScdq = 0x0400U;
    static constexpr std::uint16_t kHirqMped = 0x0800U;

    [[nodiscard]] std::optional<std::uint32_t> read(core::Address address,
                                                     std::uint8_t width);
    [[nodiscard]] bool write(core::Address address, std::uint8_t width,
                             std::uint32_t value);

    // The span overload copies; the vector overload takes ownership. The
    // default is a MODE1/2048-style data image; raw 2352/2448-byte sectors are
    // accepted when explicitly selected.
    [[nodiscard]] bool setImage(std::span<const std::byte> image,
                                std::size_t sectorSize = 2048U);
    [[nodiscard]] bool setImage(std::vector<std::byte> image,
                                std::size_t sectorSize = 2048U);
    // Mount a disc image without copying the complete file into RAM.  This
    // is important for raw MDF/ISO images, which are commonly hundreds of
    // megabytes while the Saturn CD block only exposes one sector at a time.
    [[nodiscard]] bool setImageFile(const std::filesystem::path& path,
                                    std::size_t sectorSize = 2048U);
    void clearImage() noexcept;
    [[nodiscard]] bool hasImage() const noexcept;
    [[nodiscard]] std::size_t sectorSize() const noexcept;
    [[nodiscard]] std::size_t sectorCount() const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> readSector(
        std::uint32_t lba) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> readDataSector(
        std::uint32_t lba) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> readBytes(
        std::uint64_t offset, std::size_t length) const;

    struct FileEntry {
        std::uint32_t lba{};
        std::uint32_t size{};
        bool directory{};
    };

    struct DmaTransfer {
        core::Address destination{};
        std::size_t requested{};
        std::size_t transferred{};
        std::size_t sectors{};
        bool complete{};
    };

    struct PlayTraceRecord {
        std::uint64_t sequence{};
        std::array<std::uint16_t, 4> registers{};
        std::uint32_t startFad{};
        std::uint32_t endFad{};
        std::uint32_t requested{};
        std::uint32_t available{};
        std::uint32_t playable{};
        std::uint8_t destination{0xFFU};
    };

    struct SectorTraceRecord {
        std::uint64_t sequence{};
        std::uint32_t fad{};
        std::uint32_t lba{};
        std::uint32_t playCursor{};
        std::uint8_t partition{0xFFU};
        // 0 = sector consumed by PLAY; 1 = sector fetched by GET SECTOR.
        std::uint8_t source{};
        bool retained{};
    };

    struct Stats {
        std::uint64_t commands{};
        std::uint64_t hirqReads{};
        std::uint64_t hirqWrites{};
        std::uint64_t fifoReads{};
        std::uint64_t fifoWrites{};
        std::uint64_t sectorsPrepared{};
        std::uint16_t lastHirq{};
        std::array<std::uint16_t, 32> hirqWriteHistory{};
        std::size_t hirqWriteHistoryCount{};
        std::array<std::uint64_t, 13> hirqReadFactors{};
        std::uint8_t lastInstruction{};
        std::uint8_t lastStatus{};
        std::uint8_t lastPartition{};
        std::uint16_t lastPartitionSectorCount{};
        std::uint32_t lastStreamingCursor{};
        std::uint32_t lastStreamingSectorCount{};
        std::uint64_t subcodeTicks{};
        std::uint64_t subcodeProducedTicks{};
        std::uint64_t subcodeBlockedTransferTicks{};
        std::uint64_t subcodeInactiveTicks{};
        std::uint64_t subcodeBackpressureTicks{};
        std::uint64_t subcodeInvalidDestinationTicks{};
        std::uint64_t subcodeRangeCompleteTicks{};
        bool lastSubcodeActive{};
        bool lastTransferActive{};
        std::uint64_t sectorNumberQueries{};
        std::array<std::uint16_t, 4> lastSectorNumberResponse{};
        std::uint8_t lastSectorNumberPartition{0xFFU};
        std::uint16_t lastSectorNumberHirq{};
        std::uint8_t lastStreamingFilter{0xFFU};
        std::uint8_t lastStreamingDestination{0xFFU};
        std::uint32_t lastFilterFad{};
        std::uint32_t lastFilterRange{};
        std::uint8_t lastFilterMode{};
        std::uint8_t lastFilterTrueConnection{0xFFU};
        std::uint8_t lastFilterFalseConnection{0xFFU};
        std::uint32_t lastPlayRequested{};
        std::uint32_t lastPlayAvailable{};
        std::uint16_t lastPlayRetained{};
        std::uint8_t lastPlayDestination{0xFFU};
        std::uint16_t lastPlayPartitionCountAfterTick{};
        // The most recent GET DATA transfer exposed through the FIFO.
        std::uint32_t lastTransferLba{};
        std::uint32_t lastTransferOffset{};
        std::uint32_t lastTransferCount{};
        std::uint8_t lastTransferPartition{0xFFU};
        std::uint8_t lastTransferInstruction{};
        std::uint32_t lastDeleteOffset{};
        std::uint32_t lastDeleteCount{};
        std::uint16_t lastDeleteBefore{};
        std::uint16_t lastDeleteAfter{};
        std::uint8_t lastDeletePartition{0xFFU};
        std::uint64_t deleteCommands{};
        std::uint64_t deleteWaits{};
        std::uint64_t transferSectorRequests{};
        std::uint64_t transferWaits{};
        std::uint32_t lastTransferWaitOffset{};
        std::uint32_t lastTransferWaitCount{};
        std::uint16_t lastTransferWaitRetained{};
        std::uint64_t deleteSectorRequests{};
        std::uint32_t lastDataOffset{};
        std::uint32_t lastDataCount{};
        std::uint16_t lastDataRetained{};
        std::uint8_t lastDataInstruction{};
        std::uint64_t calculateCommands{};
        std::uint64_t calculateWaits{};
        std::array<std::uint16_t, 4> lastCalculateRegisters{};
        std::uint16_t lastCalculateResult{};
        std::array<std::uint64_t, 256> instructionCounts{};
        std::array<std::uint16_t, 4> lastPlayRegisters{};
        std::array<std::array<std::uint16_t, 4>, 16> playHistory{};
        std::size_t playHistoryCount{};
        std::uint64_t playCommands{};
        std::array<std::uint8_t, 32> commandHistory{};
        std::array<std::array<std::uint16_t, 4>, 32> commandRegistersHistory{};
        std::size_t commandHistoryCount{};
    };

    [[nodiscard]] std::optional<FileEntry> findFile(std::string_view path) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> readFile(
        std::string_view path) const;

    // Executes the same 0x61 sector-read/FIFO sequence used by the CD Block
    // and writes the resulting bytes through the SH-2 memory interface. This
    // lets a destination in work RAM, VDP1 or VDP2 observe a real DMA-style
    // transfer without exposing the CD image storage to the CPU directly.
    [[nodiscard]] std::optional<DmaTransfer> dmaReadFile(
        std::string_view path, core::Address destination,
        sir::MachineState& state);

    [[nodiscard]] std::uint16_t hirq() const noexcept;
    [[nodiscard]] std::uint16_t hirqMask() const noexcept;
    [[nodiscard]] std::uint16_t commandRegister(std::size_t index) const noexcept;
    [[nodiscard]] std::uint32_t fifoOffset() const noexcept;
    // A host FIFO payload remains readable after software acknowledges the
    // DRDY interrupt bit.  SCU DMA is level-driven by the payload, not by the
    // interrupt latch, so expose that distinction to the DMA runtime.
    [[nodiscard]] bool fifoDataAvailable() const noexcept;
    // Subcode-Q is generated only while the drive is playing/streaming.  The
    // scheduler uses this gate so a mounted data disc does not generate a
    // continuous SCDQ interrupt before the game has issued PLAY/SEEK.
    [[nodiscard]] bool subcodeActive() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept;
    void setSectorTraceEnabled(bool enabled);
    [[nodiscard]] const std::vector<PlayTraceRecord>& playTrace() const noexcept;
    [[nodiscard]] const std::vector<SectorTraceRecord>& sectorTrace() const noexcept;
    [[nodiscard]] std::uint64_t droppedPlayTraceRecords() const noexcept;
    [[nodiscard]] std::uint64_t droppedSectorTraceRecords() const noexcept;
    void prepareSector(std::uint32_t lba);
    // The drive publishes a fresh subcode-Q report periodically, independent
    // of command completion. The deterministic scheduler calls this at its
    // display-time boundary so polling software observes the SCDQ factor.
    // A 75 Hz report spans two data sectors at the Saturn's 2x read speed.
    // A one-sector budget is also useful for deterministic producer steps.
    void onSubcodeTick(std::size_t sectorBudget = 2U) noexcept;

private:
    struct FilterState {
        std::uint32_t fad{};
        std::uint32_t range{};
        std::uint8_t mode{};
        std::uint8_t trueConnection{};
        std::uint8_t falseConnection{0xFFU};
        std::uint8_t channel{};
        std::uint8_t submodeMask{};
        std::uint8_t codingMask{};
        std::uint8_t file{};
        std::uint8_t submode{};
        std::uint8_t coding{};
    };

    struct RetainedFileInfo {
        std::uint32_t fad{};
        std::uint32_t size{};
        std::uint8_t unitSize{};
        std::uint8_t gapSize{};
        std::uint8_t fileNumber{};
        std::uint8_t attributes{};
    };

    [[nodiscard]] std::uint16_t readWord(std::uint32_t offset);
    void writeWord(std::uint32_t offset, std::uint16_t value);
    void executeCommand();
    void recordPlayTrace(const PlayTraceRecord& record) noexcept;
    void recordSectorTrace(std::uint32_t fad, std::uint32_t playCursor,
                           std::uint8_t partition, std::uint8_t source,
                           bool retained) noexcept;
    [[nodiscard]] std::uint16_t visibleHirq() const noexcept;
    [[nodiscard]] std::uint16_t freeBufferSectors() const noexcept;
    [[nodiscard]] std::optional<FileEntry> findDirectoryEntry(
        std::uint32_t lba, std::uint32_t size, std::string_view component) const;
    [[nodiscard]] std::vector<RetainedFileInfo> rootFileInfo() const;
    [[nodiscard]] std::vector<std::byte> encodeFileInfo(
        std::uint32_t fileId) const;
    [[nodiscard]] std::vector<std::byte> encodeToc() const;
    [[nodiscard]] std::vector<std::byte> encodeSectorForTransfer(
        std::uint32_t lba) const;
    [[nodiscard]] std::uint8_t streamingDestination() const noexcept;
    void recordStreamingFilter(std::uint8_t destination) noexcept;
    [[nodiscard]] std::size_t rootFileCount() const;
    [[nodiscard]] std::optional<std::vector<std::byte>> readImageRange(
        std::uint64_t offset, std::size_t length) const;

    std::vector<std::byte> image_;
    std::filesystem::path imagePath_;
    std::uint64_t imageFileSize_{};
    std::size_t sectorSize_{2048U};
    std::uint16_t hirq_{kHirqCmok};
    std::uint16_t hirqMask_{};
    std::uint16_t command_[4]{};
    bool commandPending_{}; // CR1 write through CR4 response read
    std::uint16_t mpegRgb_{};
    std::uint8_t status_{7U}; // no disc
    std::uint8_t getSectorLength_{};
    std::uint8_t putSectorLength_{};
    std::uint8_t cdDeviceConnection_{0xFFU};
    std::uint8_t lastBufferDestination_{0xFFU};
    // The HLE exposes the ISO-9660 root as the initially retained directory.
    // Keep the selector used by 0x70/0x71 so subsequent file commands observe
    // the same directory context as the hardware file-system engine.
    std::uint8_t directoryFilter_{};
    std::uint32_t directoryFileId_{};
    std::array<FilterState, 24> filters_{};
    std::array<std::uint16_t, 24> partitionSectorCounts_{};
    std::array<std::uint32_t, 24> partitionStartLba_{};
    // PLAY starts a producer for the selected retained partition. The
    // producer advances two data sectors per 75 Hz subcode tick and exposes them through
    // the same FIFO/DRDY path used by explicit 0x61 reads.
    std::uint8_t streamingPartition_{0xFFU};
    std::uint8_t streamingFilter_{0xFFU};
    std::uint32_t streamingCursor_{};
    std::uint32_t streamingSectorCount_{};
    std::uint32_t calculatedTransferWords_{};
    std::uint32_t currentFad_{150U};
    std::uint32_t fifoOffset_{};
    std::uint32_t transferWordsRead_{};
    bool transferActive_{};
    bool subcodeActive_{};
    bool sectorTraceEnabled_{};
    std::uint64_t playTraceCount_{};
    std::uint64_t sectorTraceCount_{};
    std::uint64_t droppedPlayTraceRecords_{};
    std::uint64_t droppedSectorTraceRecords_{};
    std::vector<PlayTraceRecord> playTrace_;
    std::vector<SectorTraceRecord> sectorTrace_;
    Stats stats_{};
    std::vector<std::byte> transfer_;
};

} // namespace saturn::platform
